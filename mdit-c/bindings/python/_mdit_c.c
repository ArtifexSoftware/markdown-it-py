/*
 * _mdit_c.c — minimal CPython extension wrapping the mdit-c engine.
 *
 * Surface (Phase 5, slice 1):
 *
 *   class MarkdownIt:
 *       def __init__(self, preset: str = "default", options: dict | None = None) -> None: ...
 *       def render(self, src: str) -> str: ...
 *       @property
 *       def options(self) -> dict: ...
 *       @options.setter
 *       def options(self, value: dict) -> None: ...
 *       def enable(self, names: str | list[str]) -> "MarkdownIt": ...
 *       def disable(self, names: str | list[str]) -> "MarkdownIt": ...
 *
 * The presets recognised today are ``default``, ``commonmark``, and
 * ``zero``. The ``options`` dict accepts the same scalar keys upstream
 * uses (``html``, ``xhtmlOut``, ``breaks``, ``linkify``, ``typographer``,
 * ``maxNesting``, ``langPrefix``, ``strikethrough_single_tilde``,
 * ``tasklists``, ``tasklists_editable``, ``alerts``); unknown keys are
 * silently ignored to match upstream's `MarkdownIt(opts)` behaviour.
 *
 * The full Python ``markdown_it.MarkdownIt`` API surface (parse,
 * tokens, rulers, plugins) is intentionally NOT implemented here — it
 * lands in a follow-up slice once the C side grows a stable token
 * accessor API.
 */

#define PY_SSIZE_T_CLEAN

/* Windows ships CPython's debug import library (python3XX_d.lib) only
 * with the SDK installer, not with the runtime — building this
 * extension in Debug otherwise fails to link. Swap `_DEBUG` off across
 * the `<Python.h>` include so the release-mode python3XX.lib is
 * picked up and the rest of the file compiles unchanged. This is the
 * canonical workaround used by SciPy / NumPy / pybind11. */
#if defined(_MSC_VER) && defined(_DEBUG)
#  define MDIT_RESTORE_DEBUG
#  undef _DEBUG
#  include <Python.h>
#  define _DEBUG
#else
#  include <Python.h>
#endif

#include <string.h>

#include "arena.h"
#include "linkifier.h"
#include "main.h"
#include "ruler.h"
#include "str.h"

/* ------------------------------------------------------------------ */
/* MarkdownIt object                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    mdit_arena arena;
    mdit_md    md;
    int        initialized;  /* 1 once mdit_md_init succeeded */
} PyMarkdownIt;

/* Apply the named preset to the wrapped mdit_md, mirroring upstream
 * Python presets. We translate the upstream JSON-ish config into the C
 * options struct + ruler tweaks. The defaults baked into mdit_md_init
 * are the "default" preset; this function only patches deviations. */
static int apply_preset(PyMarkdownIt *self, const char *preset)
{
    /* `default` matches mdit_md_init's defaults — nothing to do. */
    if (preset == NULL || strcmp(preset, "default") == 0) {
        return 0;
    }

    if (strcmp(preset, "commonmark") == 0) {
        self->md.options.max_nesting = 20;
        self->md.options.html        = true;
        self->md.options.xhtml_out   = true;
        /* CommonMark drops the GFM `table` block rule and the
         * `strikethrough` inline rules. */
        mdit_str table_name  = MDIT_STR_LIT("table");
        mdit_str strike_name = MDIT_STR_LIT("strikethrough");
        (void)mdit_ruler_disable(self->md.block.ruler,    &table_name,  1, true);
        (void)mdit_ruler_disable(self->md.inline_p.ruler, &strike_name, 1, true);
        (void)mdit_ruler_disable(self->md.inline_p.ruler2,&strike_name, 1, true);
        return 0;
    }

    if (strcmp(preset, "zero") == 0) {
        /* The zero preset disables every block + inline + ruler2 rule
         * except a small "always-on" allowlist (paragraph, text,
         * normalize, block, inline). We snapshot each ruler's current
         * names, then disable everything not in the allowlist. */
        const char *block_keep[]  = { "paragraph" };
        const char *inline_keep[] = { "text" };
        const char *core_keep[]   = { "normalize", "block", "inline" };
        const struct {
            mdit_ruler  *r;
            const char **keep;
            size_t       n_keep;
        } rulers[] = {
            { self->md.block.ruler,    block_keep,  1 },
            { self->md.inline_p.ruler, inline_keep, 1 },
            { self->md.inline_p.ruler2, NULL,       0 },
            { self->md.core.ruler,     core_keep,   3 },
        };
        for (size_t i = 0; i < sizeof rulers / sizeof *rulers; ++i) {
            size_t n = 0;
            const mdit_str *names = mdit_ruler_all_rule_names(rulers[i].r, &n);
            for (size_t j = 0; j < n; ++j) {
                int keep = 0;
                for (size_t k = 0; k < rulers[i].n_keep; ++k) {
                    if (mdit_str_eq_z(names[j], rulers[i].keep[k])) {
                        keep = 1;
                        break;
                    }
                }
                if (!keep) {
                    mdit_str name = names[j];
                    (void)mdit_ruler_disable(rulers[i].r, &name, 1, true);
                }
            }
        }
        return 0;
    }

    PyErr_Format(PyExc_ValueError,
        "unknown preset %.200s (expected 'default', 'commonmark', or 'zero')",
        preset);
    return -1;
}

/* Return 1 if obj is a Python True / non-empty number. Treats None as
 * "leave the option alone" so callers can skip keys cleanly. */
static int as_bool(PyObject *obj, int *out)
{
    if (obj == NULL || obj == Py_None) return 0;
    int r = PyObject_IsTrue(obj);
    if (r < 0) return -1;
    *out = r;
    return 1;
}

/* Apply an `options` dict on top of the current preset state. Unknown
 * keys are ignored (upstream behaviour). */
static int apply_options_dict(PyMarkdownIt *self, PyObject *opts)
{
    if (opts == NULL || opts == Py_None) return 0;
    if (!PyDict_Check(opts)) {
        PyErr_SetString(PyExc_TypeError,
            "options must be a dict or None");
        return -1;
    }

    const struct {
        const char *key;
        size_t      offset;     /* offset of bool field in mdit_options */
    } bool_keys[] = {
        { "html",                       offsetof(mdit_options, html)                       },
        { "xhtmlOut",                   offsetof(mdit_options, xhtml_out)                  },
        { "breaks",                     offsetof(mdit_options, breaks)                     },
        { "linkify",                    offsetof(mdit_options, linkify)                    },
        { "typographer",                offsetof(mdit_options, typographer)                },
        { "strikethrough_single_tilde", offsetof(mdit_options, strikethrough_single_tilde) },
        { "tasklists",                  offsetof(mdit_options, tasklists)                  },
        { "tasklists_editable",         offsetof(mdit_options, tasklists_editable)         },
        { "alerts",                     offsetof(mdit_options, alerts)                     },
    };
    for (size_t i = 0; i < sizeof bool_keys / sizeof *bool_keys; ++i) {
        PyObject *v = PyDict_GetItemString(opts, bool_keys[i].key);
        int parsed;
        int got = as_bool(v, &parsed);
        if (got < 0) return -1;
        if (got > 0) {
            bool *slot = (bool *)((char *)&self->md.options
                                  + bool_keys[i].offset);
            *slot = parsed ? true : false;
        }
    }

    PyObject *mn = PyDict_GetItemString(opts, "maxNesting");
    if (mn != NULL && mn != Py_None) {
        long val = PyLong_AsLong(mn);
        if (val == -1 && PyErr_Occurred()) return -1;
        self->md.options.max_nesting = (int32_t)val;
    }

    /* Linkify needs a registered linkifier — install the default one
     * implicitly so callers don't have to. */
    if (self->md.options.linkify && self->md.linkifier == NULL) {
        mdit_md_set_linkifier(&self->md, mdit_linkifier_default());
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* tp_init / tp_dealloc                                                */
/* ------------------------------------------------------------------ */

static int PyMarkdownIt_init(PyMarkdownIt *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = { "preset", "options", NULL };
    const char *preset    = "default";
    PyObject   *opts_obj  = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|sO", kwlist,
                                     &preset, &opts_obj)) {
        return -1;
    }

    if (!self->initialized) {
        mdit_arena_init(&self->arena, 0);
        if (!mdit_md_init(&self->md, &self->arena)) {
            mdit_arena_destroy(&self->arena);
            PyErr_SetString(PyExc_RuntimeError, "mdit_md_init failed");
            return -1;
        }
        self->initialized = 1;
    }

    if (apply_preset(self, preset) < 0) return -1;
    if (apply_options_dict(self, opts_obj) < 0) return -1;

    return 0;
}

static void PyMarkdownIt_dealloc(PyMarkdownIt *self)
{
    if (self->initialized) {
        mdit_md_destroy(&self->md);
        mdit_arena_destroy(&self->arena);
        self->initialized = 0;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* ------------------------------------------------------------------ */
/* render()                                                            */
/* ------------------------------------------------------------------ */

static PyObject *PyMarkdownIt_render(PyMarkdownIt *self, PyObject *arg)
{
    if (!self->initialized) {
        PyErr_SetString(PyExc_RuntimeError, "MarkdownIt not initialized");
        return NULL;
    }

    Py_ssize_t   n = 0;
    const char  *s = PyUnicode_AsUTF8AndSize(arg, &n);
    if (s == NULL) return NULL;

    /* Reset the arena between renders so memory doesn't grow without
     * bound across many calls on the same instance. mdit_md_init kept
     * pointers into the arena (renderer slots, ruler entries), so we
     * must NOT reset — instead we accept that long-lived instances
     * grow proportionally to total parse activity. A future slice
     * will introduce a per-render scratch arena. */
    mdit_buf out;
    mdit_buf_init(&out);
    mdit_str src = { s, (size_t)n };
    bool ok = mdit_md_render(&self->md, src, NULL, &out);
    if (!ok) {
        mdit_buf_destroy(&out);
        PyErr_SetString(PyExc_RuntimeError, "mdit_md_render failed");
        return NULL;
    }
    PyObject *result = PyUnicode_DecodeUTF8(
        (out.data ? out.data : ""), (Py_ssize_t)out.len, "strict");
    mdit_buf_destroy(&out);
    return result;
}

/* ------------------------------------------------------------------ */
/* enable() / disable()                                                */
/* ------------------------------------------------------------------ */

/* Helper: collect a single string or sequence of strings into a static
 * buffer of mdit_str views. The returned views borrow from the Python
 * objects, which the caller keeps alive across the call. */
static int collect_names(PyObject *arg, mdit_str **out_names, size_t *out_n,
                         PyObject **out_holder)
{
    *out_holder = NULL;
    if (PyUnicode_Check(arg)) {
        PyObject *list = PyList_New(1);
        if (list == NULL) return -1;
        Py_INCREF(arg);
        PyList_SET_ITEM(list, 0, arg);
        *out_holder = list;
    } else {
        PyObject *fast = PySequence_Fast(arg,
            "names must be a string or sequence of strings");
        if (fast == NULL) return -1;
        *out_holder = fast;
    }

    Py_ssize_t n = PySequence_Fast_GET_SIZE(*out_holder);
    mdit_str *buf = (mdit_str *)PyMem_Malloc((size_t)n * sizeof(mdit_str));
    if (buf == NULL) {
        Py_CLEAR(*out_holder);
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *item = PySequence_Fast_GET_ITEM(*out_holder, i);
        if (!PyUnicode_Check(item)) {
            PyMem_Free(buf);
            Py_CLEAR(*out_holder);
            PyErr_SetString(PyExc_TypeError,
                "every name must be a string");
            return -1;
        }
        Py_ssize_t  slen = 0;
        const char *s    = PyUnicode_AsUTF8AndSize(item, &slen);
        if (s == NULL) {
            PyMem_Free(buf);
            Py_CLEAR(*out_holder);
            return -1;
        }
        buf[i].data = s;
        buf[i].len  = (size_t)slen;
    }
    *out_names = buf;
    *out_n     = (size_t)n;
    return 0;
}

static PyObject *toggle_rules(PyMarkdownIt *self, PyObject *args, int enable)
{
    PyObject *names_obj = NULL;
    int       ignore    = 0;
    if (!PyArg_ParseTuple(args, "O|p", &names_obj, &ignore)) return NULL;

    mdit_str *names = NULL;
    size_t    n     = 0;
    PyObject *holder = NULL;
    if (collect_names(names_obj, &names, &n, &holder) < 0) return NULL;

    /* Try every ruler — upstream's enable/disable does the same and
     * raises only when no ruler matched any of the requested names.
     * `mdit_ruler_enable/disable` return the count of toggled rules;
     * a non-zero return on any ruler counts as a match. */
    bool any_failed = false;
    mdit_ruler *rulers[] = {
        self->md.core.ruler,
        self->md.block.ruler,
        self->md.inline_p.ruler,
        self->md.inline_p.ruler2,
    };
    for (size_t i = 0; i < n; ++i) {
        bool matched = false;
        for (size_t j = 0; j < sizeof rulers / sizeof *rulers; ++j) {
            int rc = enable
                ? mdit_ruler_enable (rulers[j], &names[i], 1, true)
                : mdit_ruler_disable(rulers[j], &names[i], 1, true);
            if (rc > 0) matched = true;
        }
        if (!matched) any_failed = true;
    }

    PyMem_Free(names);
    Py_DECREF(holder);

    if (any_failed && !ignore) {
        PyErr_Format(PyExc_ValueError,
            "%s: unknown rule name(s)", enable ? "enable" : "disable");
        return NULL;
    }

    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *PyMarkdownIt_enable(PyMarkdownIt *self, PyObject *args)
{
    return toggle_rules(self, args, 1);
}

static PyObject *PyMarkdownIt_disable(PyMarkdownIt *self, PyObject *args)
{
    return toggle_rules(self, args, 0);
}

/* ------------------------------------------------------------------ */
/* options getter / setter                                             */
/* ------------------------------------------------------------------ */

static PyObject *PyMarkdownIt_options_get(PyMarkdownIt *self, void *closure)
{
    (void)closure;
    PyObject *d = PyDict_New();
    if (d == NULL) return NULL;
#define SET_BOOL(key, field) do {                              \
        PyObject *v = (self->md.options.field) ? Py_True : Py_False; \
        if (PyDict_SetItemString(d, key, v) < 0) goto err;     \
    } while (0)
#define SET_INT(key, field) do {                               \
        PyObject *v = PyLong_FromLong(self->md.options.field); \
        if (v == NULL) goto err;                               \
        int rc = PyDict_SetItemString(d, key, v);              \
        Py_DECREF(v);                                          \
        if (rc < 0) goto err;                                  \
    } while (0)

    SET_INT ("maxNesting",                  max_nesting);
    SET_BOOL("html",                        html);
    SET_BOOL("xhtmlOut",                    xhtml_out);
    SET_BOOL("breaks",                      breaks);
    SET_BOOL("linkify",                     linkify);
    SET_BOOL("typographer",                 typographer);
    SET_BOOL("strikethrough_single_tilde",  strikethrough_single_tilde);
    SET_BOOL("tasklists",                   tasklists);
    SET_BOOL("tasklists_editable",          tasklists_editable);
    SET_BOOL("alerts",                      alerts);
    return d;
err:
    Py_DECREF(d);
    return NULL;
#undef SET_BOOL
#undef SET_INT
}

static int PyMarkdownIt_options_set(PyMarkdownIt *self, PyObject *value,
                                    void *closure)
{
    (void)closure;
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "options cannot be deleted");
        return -1;
    }
    return apply_options_dict(self, value);
}

static PyGetSetDef PyMarkdownIt_getsetters[] = {
    { "options",
      (getter)PyMarkdownIt_options_get,
      (setter)PyMarkdownIt_options_set,
      "Parser options as a dict (subset of upstream MarkdownIt.options).",
      NULL },
    { NULL }
};

static PyMethodDef PyMarkdownIt_methods[] = {
    { "render",  (PyCFunction)PyMarkdownIt_render,  METH_O,
      "render(src) -> str: parse + render Markdown to HTML." },
    { "enable",  (PyCFunction)PyMarkdownIt_enable,  METH_VARARGS,
      "enable(names, ignoreInvalid=False) -> self: enable rule(s)." },
    { "disable", (PyCFunction)PyMarkdownIt_disable, METH_VARARGS,
      "disable(names, ignoreInvalid=False) -> self: disable rule(s)." },
    { NULL }
};

static PyTypeObject PyMarkdownIt_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name        = "_mdit_c.MarkdownIt",
    .tp_basicsize   = sizeof(PyMarkdownIt),
    .tp_itemsize    = 0,
    .tp_flags       = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc         = "MarkdownIt(preset='default', options=None)\n\n"
                      "Configurable Markdown parser backed by the mdit-c engine.",
    .tp_init        = (initproc)PyMarkdownIt_init,
    .tp_dealloc     = (destructor)PyMarkdownIt_dealloc,
    .tp_methods     = PyMarkdownIt_methods,
    .tp_getset      = PyMarkdownIt_getsetters,
    .tp_new         = PyType_GenericNew,
};

/* ------------------------------------------------------------------ */
/* Module init                                                         */
/* ------------------------------------------------------------------ */

static PyModuleDef mdit_c_moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "_mdit_c",
    .m_doc     = "Internal CPython binding for the mdit-c engine. "
                 "Use the `mdit_c` package for the supported API.",
    .m_size    = -1,
};

PyMODINIT_FUNC PyInit__mdit_c(void)
{
    if (PyType_Ready(&PyMarkdownIt_Type) < 0) return NULL;

    PyObject *m = PyModule_Create(&mdit_c_moduledef);
    if (m == NULL) return NULL;

    Py_INCREF(&PyMarkdownIt_Type);
    if (PyModule_AddObject(m, "MarkdownIt",
                           (PyObject *)&PyMarkdownIt_Type) < 0) {
        Py_DECREF(&PyMarkdownIt_Type);
        Py_DECREF(m);
        return NULL;
    }

    /* Module version mirrors the C library version. */
    PyModule_AddStringConstant(m, "__version__", "0.0.1");
    return m;
}
