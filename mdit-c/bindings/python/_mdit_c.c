/*
 * _mdit_c.c — minimal CPython extension wrapping the mdit-c engine.
 *
 * Surface (Phase 5, slices 1–3):
 *
 *   class MarkdownIt:
 *       def __init__(self, preset: str = "commonmark", options: dict | None = None) -> None: ...
 *       def parse(self, src: str, env: object | None = None) -> list[Token]: ...
 *       def render(self, src: str) -> str: ...
 *       @property
 *       def options(self) -> dict: ...
 *       @options.setter
 *       def options(self, value: dict) -> None: ...
 *       def enable(self, names: str | list[str]) -> "MarkdownIt": ...
 *       def disable(self, names: str | list[str]) -> "MarkdownIt": ...
 *
 * The default preset matches upstream's
 * ``markdown_it.MarkdownIt(config="commonmark")`` behaviour. Other
 * recognised preset names: ``default`` / ``js-default``, ``zero``,
 * ``gfm-like``, ``gfm-like2``. The ``options`` dict accepts the same
 * keys upstream uses (``html``, ``xhtmlOut``, ``breaks``, ``linkify``,
 * ``typographer``, ``maxNesting``, ``langPrefix``, ``quotes``,
 * ``strikethrough_single_tilde``, ``tasklists``, ``tasklists_editable``,
 * ``alerts``). Mutations of the live ``md.options`` dict propagate
 * into the C engine on the next parse/render call. Unknown keys are
 * silently ignored to match upstream's ``MarkdownIt(options=...)``
 * behaviour.
 *
 * The full Python ``markdown_it.MarkdownIt`` API surface (rulers,
 * plugins, renderer customisation, SyntaxTreeNode) is intentionally NOT
 * implemented here — those land in follow-up slices once the C side
 * grows stable public accessors for the remaining pieces.
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

#include <stddef.h>
#include <string.h>

#include "arena.h"
#include "env.h"
#include "linkifier.h"
#include "main.h"
#include "ruler.h"
#include "str.h"
#include "token.h"

#include <structmember.h>

/* ------------------------------------------------------------------ */
/* Token object                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    PyObject *type;
    PyObject *tag;
    int       nesting;
    PyObject *attrs;
    PyObject *map;
    int       level;
    PyObject *children;
    PyObject *content;
    PyObject *markup;
    PyObject *info;
    PyObject *meta;
    int       block;
    int       hidden;
} PyToken;

static PyTypeObject PyToken_Type;

static PyObject *py_from_mdit_str(mdit_str s)
{
    return PyUnicode_DecodeUTF8(s.data ? s.data : "", (Py_ssize_t)s.len,
                                "strict");
}

static PyObject *py_from_value(const mdit_value *v)
{
    switch (v->kind) {
        case MDIT_VALUE_NULL:
            Py_RETURN_NONE;
        case MDIT_VALUE_BOOL:
            return PyBool_FromLong(v->u.b ? 1 : 0);
        case MDIT_VALUE_INT:
            return PyLong_FromLongLong((long long)v->u.i);
        case MDIT_VALUE_DOUBLE:
            return PyFloat_FromDouble(v->u.d);
        case MDIT_VALUE_STR:
            return py_from_mdit_str(v->u.s);
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown mdit_value kind");
    return NULL;
}

static PyObject *py_map_to_dict(const mdit_map *m)
{
    PyObject *d = PyDict_New();
    if (d == NULL) return NULL;
    for (size_t i = 0; i < mdit_map_len(m); ++i) {
        const mdit_map_entry *e = mdit_map_at(m, i);
        PyObject *key = py_from_mdit_str(e->key);
        PyObject *val = py_from_value(&e->value);
        if (key == NULL || val == NULL ||
            PyDict_SetItem(d, key, val) < 0) {
            Py_XDECREF(key);
            Py_XDECREF(val);
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(key);
        Py_DECREF(val);
    }
    return d;
}

static PyObject *py_attrs_as_upstream(PyObject *attrs)
{
    Py_ssize_t n = PyDict_Size(attrs);
    if (n == 0) Py_RETURN_NONE;

    PyObject *items = PyList_New(n);
    if (items == NULL) return NULL;

    Py_ssize_t pos = 0;
    Py_ssize_t out_i = 0;
    PyObject *key = NULL;
    PyObject *val = NULL;
    while (PyDict_Next(attrs, &pos, &key, &val)) {
        PyObject *pair = PyList_New(2);
        if (pair == NULL) {
            Py_DECREF(items);
            return NULL;
        }
        Py_INCREF(key);
        Py_INCREF(val);
        PyList_SET_ITEM(pair, 0, key);
        PyList_SET_ITEM(pair, 1, val);
        PyList_SET_ITEM(items, out_i++, pair);
    }
    return items;
}

static PyObject *PyToken_as_dict(PyToken *self, PyObject *args,
                                 PyObject *kwargs)
{
    static char *kwlist[] = {
        "children", "as_upstream", "meta_serializer", "filter",
        "dict_factory", NULL
    };
    int children_flag = 1;
    int as_upstream = 1;
    PyObject *meta_serializer = Py_None;
    PyObject *filter = Py_None;
    PyObject *dict_factory = (PyObject *)&PyDict_Type;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ppOOO", kwlist,
                                     &children_flag, &as_upstream,
                                     &meta_serializer, &filter,
                                     &dict_factory)) {
        return NULL;
    }
    if (!PyCallable_Check(dict_factory)) {
        PyErr_SetString(PyExc_TypeError, "dict_factory must be callable");
        return NULL;
    }

    PyObject *d = PyObject_CallNoArgs(dict_factory);
    if (d == NULL) return NULL;

#define SET_ITEM(name, value) do {                                      \
        PyObject *_key = PyUnicode_FromString(name);                    \
        PyObject *_val = (value);                                       \
        if (_key == NULL || _val == NULL) {                             \
            Py_XDECREF(_key);                                           \
            Py_XDECREF(_val);                                           \
            Py_DECREF(d);                                               \
            return NULL;                                                \
        }                                                               \
        int _keep = 1;                                                  \
        if (filter != Py_None) {                                        \
            PyObject *_ok = PyObject_CallFunctionObjArgs(               \
                filter, _key, _val, NULL);                              \
            if (_ok == NULL) {                                          \
                Py_DECREF(_key); Py_DECREF(_val); Py_DECREF(d);         \
                return NULL;                                            \
            }                                                           \
            _keep = PyObject_IsTrue(_ok);                               \
            Py_DECREF(_ok);                                             \
            if (_keep < 0) {                                            \
                Py_DECREF(_key); Py_DECREF(_val); Py_DECREF(d);         \
                return NULL;                                            \
            }                                                           \
        }                                                               \
        if (_keep && PyObject_SetItem(d, _key, _val) < 0) {             \
            Py_DECREF(_key); Py_DECREF(_val); Py_DECREF(d);             \
            return NULL;                                                \
        }                                                               \
        Py_DECREF(_key);                                                \
        Py_DECREF(_val);                                                \
    } while (0)
#define SET_BORROWED(name, obj) do { Py_INCREF(obj); SET_ITEM(name, obj); } while (0)
#define SET_LONG(name, v) SET_ITEM(name, PyLong_FromLong((long)(v)))
#define SET_BOOL(name, v) SET_ITEM(name, PyBool_FromLong((v) ? 1 : 0))

    SET_BORROWED("type", self->type);
    SET_BORROWED("tag", self->tag);
    SET_LONG("nesting", self->nesting);
    if (as_upstream) {
        SET_ITEM("attrs", py_attrs_as_upstream(self->attrs));
    } else {
        SET_BORROWED("attrs", self->attrs);
    }
    SET_BORROWED("map", self->map);
    SET_LONG("level", self->level);

    if (children_flag && self->children != Py_None &&
        PyList_Check(self->children) && PyList_GET_SIZE(self->children) > 0) {
        Py_ssize_t n = PyList_GET_SIZE(self->children);
        PyObject *child_dicts = PyList_New(n);
        if (child_dicts == NULL) { Py_DECREF(d); return NULL; }
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *child = PyList_GET_ITEM(self->children, i);
            PyObject *kw = Py_BuildValue(
                "{s:O,s:O,s:O,s:O,s:O}",
                "children", children_flag ? Py_True : Py_False,
                "as_upstream", as_upstream ? Py_True : Py_False,
                "meta_serializer", meta_serializer,
                "filter", filter,
                "dict_factory", dict_factory);
            if (kw == NULL) {
                Py_DECREF(child_dicts); Py_DECREF(d); return NULL;
            }
            PyObject *method = PyObject_GetAttrString(child, "as_dict");
            PyObject *empty_args = PyTuple_New(0);
            if (method == NULL || empty_args == NULL) {
                Py_XDECREF(method);
                Py_XDECREF(empty_args);
                Py_DECREF(kw);
                Py_DECREF(child_dicts); Py_DECREF(d); return NULL;
            }
            PyObject *converted = PyObject_Call(method, empty_args, kw);
            Py_DECREF(method);
            Py_DECREF(empty_args);
            Py_DECREF(kw);
            if (converted == NULL) {
                Py_DECREF(child_dicts); Py_DECREF(d); return NULL;
            }
            PyList_SET_ITEM(child_dicts, i, converted);
        }
        SET_ITEM("children", child_dicts);
    } else {
        SET_BORROWED("children", self->children);
    }

    SET_BORROWED("content", self->content);
    SET_BORROWED("markup", self->markup);
    SET_BORROWED("info", self->info);
    if (meta_serializer != Py_None) {
        PyObject *meta = PyObject_CallFunctionObjArgs(meta_serializer,
                                                      self->meta, NULL);
        SET_ITEM("meta", meta);
    } else {
        SET_BORROWED("meta", self->meta);
    }
    SET_BOOL("block", self->block);
    SET_BOOL("hidden", self->hidden);

    return d;

#undef SET_BOOL
#undef SET_LONG
#undef SET_BORROWED
#undef SET_ITEM
}

static void PyToken_dealloc(PyToken *self)
{
    Py_XDECREF(self->type);
    Py_XDECREF(self->tag);
    Py_XDECREF(self->attrs);
    Py_XDECREF(self->map);
    Py_XDECREF(self->children);
    Py_XDECREF(self->content);
    Py_XDECREF(self->markup);
    Py_XDECREF(self->info);
    Py_XDECREF(self->meta);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *PyToken_attrIndex(PyToken *self, PyObject *arg)
{
    Py_ssize_t pos = 0;
    Py_ssize_t idx = 0;
    PyObject *key = NULL;
    PyObject *val = NULL;
    while (PyDict_Next(self->attrs, &pos, &key, &val)) {
        int eq = PyObject_RichCompareBool(key, arg, Py_EQ);
        if (eq < 0) return NULL;
        if (eq) return PyLong_FromSsize_t(idx);
        ++idx;
    }
    return PyLong_FromLong(-1);
}

static PyObject *PyToken_attrItems(PyToken *self, PyObject *Py_UNUSED(ignored))
{
    return PyMapping_Items(self->attrs);
}

static PyObject *PyToken_attrGet(PyToken *self, PyObject *arg)
{
    PyObject *v = PyDict_GetItemWithError(self->attrs, arg);
    if (v == NULL) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    Py_INCREF(v);
    return v;
}

static PyObject *PyToken_attrSet(PyToken *self, PyObject *args)
{
    PyObject *name = NULL;
    PyObject *value = NULL;
    if (!PyArg_ParseTuple(args, "OO", &name, &value)) return NULL;
    if (PyDict_SetItem(self->attrs, name, value) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *PyToken_attrPush(PyToken *self, PyObject *arg)
{
    PyObject *seq = PySequence_Fast(arg, "attrPush expects a (name, value) pair");
    if (seq == NULL) return NULL;
    if (PySequence_Fast_GET_SIZE(seq) != 2) {
        Py_DECREF(seq);
        PyErr_SetString(PyExc_ValueError, "attrPush expects a pair");
        return NULL;
    }
    PyObject *name = PySequence_Fast_GET_ITEM(seq, 0);
    PyObject *value = PySequence_Fast_GET_ITEM(seq, 1);
    int rc = PyDict_SetItem(self->attrs, name, value);
    Py_DECREF(seq);
    if (rc < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *PyToken_attrJoin(PyToken *self, PyObject *args)
{
    PyObject *name = NULL;
    const char *value = NULL;
    Py_ssize_t value_len = 0;
    if (!PyArg_ParseTuple(args, "Os#", &name, &value, &value_len)) return NULL;

    PyObject *existing = PyDict_GetItemWithError(self->attrs, name);
    if (existing == NULL) {
        if (PyErr_Occurred()) return NULL;
        PyObject *v = PyUnicode_DecodeUTF8(value, value_len, "strict");
        if (v == NULL) return NULL;
        int rc = PyDict_SetItem(self->attrs, name, v);
        Py_DECREF(v);
        if (rc < 0) return NULL;
        Py_RETURN_NONE;
    }
    if (!PyUnicode_Check(existing)) {
        PyErr_SetString(PyExc_TypeError,
            "existing attr 'name' is not a str");
        return NULL;
    }
    PyObject *sep = PyUnicode_FromString(" ");
    PyObject *tail = PyUnicode_DecodeUTF8(value, value_len, "strict");
    PyObject *joined = NULL;
    if (sep != NULL && tail != NULL) {
        PyObject *tmp = PyUnicode_Concat(existing, sep);
        if (tmp != NULL) {
            joined = PyUnicode_Concat(tmp, tail);
            Py_DECREF(tmp);
        }
    }
    Py_XDECREF(sep);
    Py_XDECREF(tail);
    if (joined == NULL) return NULL;
    int rc = PyDict_SetItem(self->attrs, name, joined);
    Py_DECREF(joined);
    if (rc < 0) return NULL;
    Py_RETURN_NONE;
}

static PyMemberDef PyToken_members[] = {
    { "type",     T_OBJECT_EX, offsetof(PyToken, type),     0, "token type" },
    { "tag",      T_OBJECT_EX, offsetof(PyToken, tag),      0, "HTML tag" },
    { "nesting",  T_INT,       offsetof(PyToken, nesting),  0, "nesting" },
    { "attrs",    T_OBJECT_EX, offsetof(PyToken, attrs),    0, "attrs dict" },
    { "map",      T_OBJECT_EX, offsetof(PyToken, map),      0, "source map" },
    { "level",    T_INT,       offsetof(PyToken, level),    0, "level" },
    { "children", T_OBJECT_EX, offsetof(PyToken, children), 0, "children" },
    { "content",  T_OBJECT_EX, offsetof(PyToken, content),  0, "content" },
    { "markup",   T_OBJECT_EX, offsetof(PyToken, markup),   0, "markup" },
    { "info",     T_OBJECT_EX, offsetof(PyToken, info),     0, "info" },
    { "meta",     T_OBJECT_EX, offsetof(PyToken, meta),     0, "meta dict" },
    { "block",    T_BOOL,      offsetof(PyToken, block),    0, "block" },
    { "hidden",   T_BOOL,      offsetof(PyToken, hidden),   0, "hidden" },
    { NULL }
};

static int PyToken_init(PyToken *self, PyObject *args, PyObject *kwargs);
static PyObject *PyToken_richcompare(PyObject *a, PyObject *b, int op);
static PyObject *PyToken_from_dict(PyTypeObject *cls, PyObject *dict_obj);
static PyObject *PyToken_copy(PyToken *self, PyObject *args, PyObject *kwargs);

static PyMethodDef PyToken_methods[] = {
    { "as_dict", (PyCFunction)PyToken_as_dict,
      METH_VARARGS | METH_KEYWORDS,
      "as_dict(*, children=True, as_upstream=True, meta_serializer=None, "
      "filter=None, dict_factory=dict)" },
    { "attrIndex", (PyCFunction)PyToken_attrIndex, METH_O,
      "attrIndex(name) -> int" },
    { "attrItems", (PyCFunction)PyToken_attrItems, METH_NOARGS,
      "attrItems() -> list[tuple[str, value]]" },
    { "attrGet", (PyCFunction)PyToken_attrGet, METH_O,
      "attrGet(name) -> value | None" },
    { "attrSet", (PyCFunction)PyToken_attrSet, METH_VARARGS,
      "attrSet(name, value) -> None" },
    { "attrPush", (PyCFunction)PyToken_attrPush, METH_O,
      "attrPush((name, value)) -> None" },
    { "attrJoin", (PyCFunction)PyToken_attrJoin, METH_VARARGS,
      "attrJoin(name, value) -> None" },
    { "copy", (PyCFunction)PyToken_copy, METH_VARARGS | METH_KEYWORDS,
      "copy(**changes) -> Token: shallow copy with optional overrides." },
    { "from_dict", (PyCFunction)PyToken_from_dict,
      METH_O | METH_CLASS,
      "from_dict(d) -> Token: rebuild a Token from `as_dict()` output." },
    { NULL }
};

static PyTypeObject PyToken_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name        = "_mdit_c.Token",
    .tp_basicsize   = sizeof(PyToken),
    .tp_flags       = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc         = "Token copied from the mdit-c token stream.",
    .tp_dealloc     = (destructor)PyToken_dealloc,
    .tp_members     = PyToken_members,
    .tp_methods     = PyToken_methods,
    .tp_init        = (initproc)PyToken_init,
    .tp_new         = PyType_GenericNew,
    .tp_richcompare = PyToken_richcompare,
};

static PyObject *PyToken_from_mdit(const mdit_token *t);

static int token_set_defaults(PyToken *self)
{
    self->type = PyUnicode_FromString("");
    self->tag = PyUnicode_FromString("");
    self->nesting = 0;
    self->attrs = PyDict_New();
    Py_INCREF(Py_None);
    self->map = Py_None;
    self->level = 0;
    Py_INCREF(Py_None);
    self->children = Py_None;
    self->content = PyUnicode_FromString("");
    self->markup = PyUnicode_FromString("");
    self->info = PyUnicode_FromString("");
    self->meta = PyDict_New();
    self->block = 0;
    self->hidden = 0;
    if (self->type == NULL || self->tag == NULL || self->attrs == NULL ||
        self->content == NULL || self->markup == NULL || self->info == NULL ||
        self->meta == NULL) {
        return -1;
    }
    return 0;
}

static int PyToken_init(PyToken *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {
        "type", "tag", "nesting", "attrs", "map", "level", "children",
        "content", "markup", "info", "meta", "block", "hidden", NULL
    };
    PyObject *type = NULL;
    PyObject *tag = NULL;
    int nesting = 0;
    PyObject *attrs = Py_None;
    PyObject *map = Py_None;
    int level = 0;
    PyObject *children = Py_None;
    PyObject *content = NULL;
    PyObject *markup = NULL;
    PyObject *info = NULL;
    PyObject *meta = Py_None;
    int block = 0;
    int hidden = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "UUi|OOiOUUUOpp",
                                     kwlist, &type, &tag, &nesting,
                                     &attrs, &map, &level, &children,
                                     &content, &markup, &info, &meta,
                                     &block, &hidden)) {
        return -1;
    }
    if (nesting < -1 || nesting > 1) {
        PyErr_SetString(PyExc_ValueError,
                        "nesting must be in {-1, 0, 1}");
        return -1;
    }

    /* attrs: None | dict | list[[k,v], ...]  →  owned dict (convert_attrs). */
    PyObject *attrs_dict;
    if (attrs == Py_None) {
        attrs_dict = PyDict_New();
    } else if (PyDict_Check(attrs)) {
        attrs_dict = PyDict_Copy(attrs);
    } else if (PyList_Check(attrs) || PyTuple_Check(attrs)) {
        attrs_dict = PyObject_CallFunctionObjArgs(
            (PyObject *)&PyDict_Type, attrs, NULL);
    } else {
        PyErr_SetString(PyExc_TypeError,
            "attrs must be None, a dict, or a list of [key, value] pairs");
        return -1;
    }
    if (attrs_dict == NULL) return -1;

    /* meta: None | dict → owned dict. */
    PyObject *meta_dict;
    if (meta == Py_None) {
        meta_dict = PyDict_New();
    } else if (PyDict_Check(meta)) {
        meta_dict = PyDict_Copy(meta);
    } else {
        Py_DECREF(attrs_dict);
        PyErr_SetString(PyExc_TypeError, "meta must be None or a dict");
        return -1;
    }
    if (meta_dict == NULL) {
        Py_DECREF(attrs_dict);
        return -1;
    }

    /* map: None | sequence-of-int → None | list[int]. */
    PyObject *map_value;
    if (map == Py_None) {
        Py_INCREF(Py_None);
        map_value = Py_None;
    } else {
        map_value = PySequence_List(map);
        if (map_value == NULL) {
            Py_DECREF(attrs_dict);
            Py_DECREF(meta_dict);
            return -1;
        }
    }

    /* children: None | sequence-of-Token → None | list[Token]. */
    PyObject *children_value;
    if (children == Py_None) {
        Py_INCREF(Py_None);
        children_value = Py_None;
    } else {
        children_value = PySequence_List(children);
        if (children_value == NULL) {
            Py_DECREF(attrs_dict);
            Py_DECREF(meta_dict);
            Py_DECREF(map_value);
            return -1;
        }
    }

    /* Default empty strings for content/markup/info if omitted. */
    PyObject *content_value = content;
    PyObject *markup_value  = markup;
    PyObject *info_value    = info;
    if (content_value == NULL) content_value = PyUnicode_FromString("");
    else                       Py_INCREF(content_value);
    if (markup_value  == NULL) markup_value  = PyUnicode_FromString("");
    else                       Py_INCREF(markup_value);
    if (info_value    == NULL) info_value    = PyUnicode_FromString("");
    else                       Py_INCREF(info_value);
    if (content_value == NULL || markup_value == NULL || info_value == NULL) {
        Py_DECREF(attrs_dict); Py_DECREF(meta_dict);
        Py_DECREF(map_value); Py_DECREF(children_value);
        Py_XDECREF(content_value); Py_XDECREF(markup_value); Py_XDECREF(info_value);
        return -1;
    }

    Py_INCREF(type);
    Py_INCREF(tag);
    Py_XSETREF(self->type, type);
    Py_XSETREF(self->tag, tag);
    self->nesting = nesting;
    Py_XSETREF(self->attrs, attrs_dict);
    Py_XSETREF(self->map, map_value);
    self->level = level;
    Py_XSETREF(self->children, children_value);
    Py_XSETREF(self->content, content_value);
    Py_XSETREF(self->markup, markup_value);
    Py_XSETREF(self->info, info_value);
    Py_XSETREF(self->meta, meta_dict);
    self->block = block ? 1 : 0;
    self->hidden = hidden ? 1 : 0;
    return 0;
}

/* Equality / inequality compare token instances by every field. Python
 * tests rely on this: ``Token.from_dict(token.as_dict()) == token``. */
static PyObject *PyToken_richcompare(PyObject *a, PyObject *b, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyObject_TypeCheck(a, &PyToken_Type) ||
        !PyObject_TypeCheck(b, &PyToken_Type)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    PyToken *x = (PyToken *)a;
    PyToken *y = (PyToken *)b;
    int eq = 1;
#define CMP_OBJ(field) do {                                            \
        int rc = PyObject_RichCompareBool(x->field, y->field, Py_EQ);  \
        if (rc < 0) return NULL;                                       \
        if (!rc) eq = 0;                                               \
    } while (0)
    CMP_OBJ(type); CMP_OBJ(tag); CMP_OBJ(attrs); CMP_OBJ(map);
    CMP_OBJ(children); CMP_OBJ(content); CMP_OBJ(markup);
    CMP_OBJ(info); CMP_OBJ(meta);
#undef CMP_OBJ
    if (x->nesting != y->nesting) eq = 0;
    if (x->level   != y->level)   eq = 0;
    if (x->block   != y->block)   eq = 0;
    if (x->hidden  != y->hidden)  eq = 0;
    if (op == Py_NE) eq = !eq;
    if (eq) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

/* `Token.from_dict(d)` — recursively reconstruct from an as_dict()-like
 * mapping, including children. The dict is shallow-copied because we
 * mutate the `children` key (replace dicts with Token instances) before
 * forwarding it as kwargs. */
static PyObject *PyToken_from_dict(PyTypeObject *cls, PyObject *dict_obj)
{
    if (!PyDict_Check(dict_obj)) {
        PyErr_SetString(PyExc_TypeError, "from_dict: expected a dict");
        return NULL;
    }
    PyObject *args = PyTuple_New(0);
    PyObject *kwargs = PyDict_Copy(dict_obj);
    if (args == NULL || kwargs == NULL) {
        Py_XDECREF(args); Py_XDECREF(kwargs);
        return NULL;
    }
    PyObject *children_obj = PyDict_GetItemString(kwargs, "children");
    if (children_obj != NULL && children_obj != Py_None) {
        Py_ssize_t n = PyObject_Length(children_obj);
        if (n < 0) { Py_DECREF(args); Py_DECREF(kwargs); return NULL; }
        PyObject *converted = PyList_New(n);
        if (converted == NULL) {
            Py_DECREF(args); Py_DECREF(kwargs); return NULL;
        }
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *raw = PySequence_GetItem(children_obj, i);
            if (raw == NULL) {
                Py_DECREF(converted); Py_DECREF(args); Py_DECREF(kwargs);
                return NULL;
            }
            PyObject *child;
            if (PyDict_Check(raw)) {
                child = PyToken_from_dict(cls, raw);
            } else {
                Py_INCREF(raw);
                child = raw;
            }
            Py_DECREF(raw);
            if (child == NULL) {
                Py_DECREF(converted); Py_DECREF(args); Py_DECREF(kwargs);
                return NULL;
            }
            PyList_SET_ITEM(converted, i, child);
        }
        if (PyDict_SetItemString(kwargs, "children", converted) < 0) {
            Py_DECREF(converted); Py_DECREF(args); Py_DECREF(kwargs);
            return NULL;
        }
        Py_DECREF(converted);
    }
    PyObject *obj = PyObject_Call((PyObject *)cls, args, kwargs);
    Py_DECREF(args);
    Py_DECREF(kwargs);
    return obj;
}

/* Shallow copy with optional field overrides — mirrors upstream
 * ``Token.copy(**changes)`` (built on dataclasses.replace). */
static PyObject *PyToken_copy(PyToken *self, PyObject *args, PyObject *kwargs)
{
    if (args != NULL && PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError,
            "Token.copy() takes only keyword arguments");
        return NULL;
    }
    PyObject *base = PyDict_New();
    if (base == NULL) return NULL;
#define BASE_BORROWED(name, obj) do {                                   \
        if (PyDict_SetItemString(base, name, obj) < 0) {                \
            Py_DECREF(base); return NULL;                               \
        }                                                               \
    } while (0)
    BASE_BORROWED("type", self->type);
    BASE_BORROWED("tag", self->tag);
    PyObject *nesting = PyLong_FromLong(self->nesting);
    if (nesting == NULL) { Py_DECREF(base); return NULL; }
    if (PyDict_SetItemString(base, "nesting", nesting) < 0) {
        Py_DECREF(nesting); Py_DECREF(base); return NULL;
    }
    Py_DECREF(nesting);
    BASE_BORROWED("attrs", self->attrs);
    BASE_BORROWED("map", self->map);
    PyObject *level = PyLong_FromLong(self->level);
    if (level == NULL) { Py_DECREF(base); return NULL; }
    if (PyDict_SetItemString(base, "level", level) < 0) {
        Py_DECREF(level); Py_DECREF(base); return NULL;
    }
    Py_DECREF(level);
    BASE_BORROWED("children", self->children);
    BASE_BORROWED("content",  self->content);
    BASE_BORROWED("markup",   self->markup);
    BASE_BORROWED("info",     self->info);
    BASE_BORROWED("meta",     self->meta);
    PyObject *block_obj = self->block ? Py_True : Py_False;
    PyObject *hidden_obj = self->hidden ? Py_True : Py_False;
    BASE_BORROWED("block",  block_obj);
    BASE_BORROWED("hidden", hidden_obj);
#undef BASE_BORROWED
    if (kwargs != NULL && PyDict_Update(base, kwargs) < 0) {
        Py_DECREF(base); return NULL;
    }
    PyObject *empty = PyTuple_New(0);
    PyObject *result = NULL;
    if (empty != NULL) {
        result = PyObject_Call((PyObject *)Py_TYPE(self), empty, base);
        Py_DECREF(empty);
    }
    Py_DECREF(base);
    return result;
}

static PyObject *py_children_from_mdit(const mdit_token *t)
{
    if (t->children == NULL) Py_RETURN_NONE;
    PyObject *list = PyList_New((Py_ssize_t)t->children_len);
    if (list == NULL) return NULL;
    for (size_t i = 0; i < t->children_len; ++i) {
        PyObject *child = PyToken_from_mdit(&t->children[i]);
        if (child == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, child);
    }
    return list;
}

static PyObject *PyToken_from_mdit(const mdit_token *t)
{
    PyToken *obj = PyObject_New(PyToken, &PyToken_Type);
    if (obj == NULL) return NULL;
    memset((char *)obj + sizeof(PyObject), 0, sizeof(*obj) - sizeof(PyObject));

    obj->type     = py_from_mdit_str(t->type);
    obj->tag      = py_from_mdit_str(t->tag);
    obj->nesting  = (int)t->nesting;
    obj->attrs    = py_map_to_dict(&t->attrs);
    obj->level    = (int)t->level;
    obj->children = py_children_from_mdit(t);
    obj->content  = py_from_mdit_str(t->content);
    obj->markup   = py_from_mdit_str(t->markup);
    obj->info     = py_from_mdit_str(t->info);
    obj->meta     = py_map_to_dict(&t->meta);
    obj->block    = t->block ? 1 : 0;
    obj->hidden   = t->hidden ? 1 : 0;

    if (t->has_map) {
        obj->map = Py_BuildValue("[ii]", (int)t->map.begin, (int)t->map.end);
    } else {
        Py_INCREF(Py_None);
        obj->map = Py_None;
    }

    if (obj->type == NULL || obj->tag == NULL || obj->attrs == NULL ||
        obj->map == NULL || obj->children == NULL || obj->content == NULL ||
        obj->markup == NULL || obj->info == NULL || obj->meta == NULL) {
        Py_DECREF(obj);
        return NULL;
    }
    return (PyObject *)obj;
}

/* ------------------------------------------------------------------ */
/* MarkdownIt object                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    mdit_arena arena;
    mdit_md    md;
    int        initialized;     /* 1 once mdit_md_init succeeded */
    PyObject  *options_dict;    /* persistent live mirror of options */
    PyObject  *render_callbacks;/* token type -> Python render callback */
} PyMarkdownIt;

typedef struct {
    PyMarkdownIt *md;
    PyObject     *env;
} PyRenderContext;

static int build_options_dict(PyMarkdownIt *self);

static bool py_render_rule_bridge(
    mdit_renderer *r,
    const mdit_token *tokens, size_t n_tokens, size_t idx,
    const mdit_renderer_options *opts,
    void *env,
    mdit_buf *out)
{
    (void)opts;
    PyRenderContext *ctx = (PyRenderContext *)env;
    if (ctx == NULL || ctx->md == NULL || ctx->md->render_callbacks == NULL) {
        return mdit_renderer_render_token(r, tokens, n_tokens, idx, opts, env, out);
    }

    PyObject *key = PyUnicode_FromStringAndSize(
        tokens[idx].type.data, (Py_ssize_t)tokens[idx].type.len);
    if (key == NULL) return false;
    PyObject *callback = PyDict_GetItemWithError(ctx->md->render_callbacks, key);
    Py_DECREF(key);
    if (callback == NULL) {
        if (PyErr_Occurred()) return false;
        return mdit_renderer_render_token(r, tokens, n_tokens, idx, opts, env, out);
    }

    PyObject *py_tokens = PyList_New((Py_ssize_t)n_tokens);
    if (py_tokens == NULL) return false;
    for (size_t i = 0; i < n_tokens; ++i) {
        PyObject *tok = PyToken_from_mdit(&tokens[i]);
        if (tok == NULL) {
            Py_DECREF(py_tokens);
            return false;
        }
        PyList_SET_ITEM(py_tokens, (Py_ssize_t)i, tok);
    }

    PyObject *py_idx = PyLong_FromSize_t(idx);
    if (py_idx == NULL) {
        Py_DECREF(py_tokens);
        return false;
    }
    if (ctx->md->options_dict == NULL && build_options_dict(ctx->md) < 0) {
        Py_DECREF(py_idx);
        Py_DECREF(py_tokens);
        return false;
    }
    PyObject *py_env = (ctx->env == NULL) ? Py_None : ctx->env;
    PyObject *result = PyObject_CallFunctionObjArgs(
        callback, py_tokens, py_idx, ctx->md->options_dict, py_env, NULL);
    Py_DECREF(py_idx);
    Py_DECREF(py_tokens);
    if (result == NULL) return false;
    if (!PyUnicode_Check(result)) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_TypeError, "render rule must return a str");
        return false;
    }
    Py_ssize_t n = 0;
    const char *s = PyUnicode_AsUTF8AndSize(result, &n);
    if (s == NULL) {
        Py_DECREF(result);
        return false;
    }
    bool ok = mdit_buf_append(out, s, (size_t)n);
    Py_DECREF(result);
    return ok;
}

/* Apply the named preset to the wrapped mdit_md, mirroring upstream
 * Python presets. We translate the upstream JSON-ish config into the C
 * options struct + ruler tweaks. The defaults baked into mdit_md_init
 * are the "default" preset; this function only patches deviations. */
static int apply_preset(PyMarkdownIt *self, const char *preset)
{
    /* `default` matches mdit_md_init's defaults — nothing to do. */
    if (preset == NULL ||
        strcmp(preset, "default") == 0 ||
        strcmp(preset, "js-default") == 0 ||
        strcmp(preset, "js_default") == 0) {
        return 0;
    }

    if (strcmp(preset, "commonmark") == 0) {
        self->md.options.max_nesting = 20;
        self->md.options.html        = true;
        self->md.options.xhtml_out   = true;
        /* CommonMark drops the GFM `table` block rule and the
         * `strikethrough` inline rules, and the optional typographer /
         * linkify core rules. */
        mdit_str table_name  = MDIT_STR_LIT("table");
        mdit_str inline_optional[] = {
            MDIT_STR_LIT("linkify"),
            MDIT_STR_LIT("strikethrough"),
        };
        mdit_str core_optional[] = {
            MDIT_STR_LIT("linkify"),
            MDIT_STR_LIT("replacements"),
            MDIT_STR_LIT("smartquotes"),
        };
        (void)mdit_ruler_disable(self->md.block.ruler, &table_name, 1, true);
        (void)mdit_ruler_disable(self->md.inline_p.ruler, inline_optional,
                                 sizeof inline_optional / sizeof *inline_optional,
                                 true);
        (void)mdit_ruler_disable(self->md.inline_p.ruler2, &inline_optional[1],
                                 1, true);
        (void)mdit_ruler_disable(self->md.core.ruler, core_optional,
                                 sizeof core_optional / sizeof *core_optional,
                                 true);
        return 0;
    }

    /* gfm-like and gfm-like2 mirror markdown_it.presets.gfm_like /
     * gfm_like2: start from CommonMark, re-enable the GFM `table` +
     * `strikethrough` rules (which `commonmark` had stripped), and turn
     * on `linkify` + `html`. gfm-like2 layers on the markdown-it-py
     * additions: tasklists, alerts, single-tilde strikethrough. */
    if (strcmp(preset, "gfm-like")  == 0 ||
        strcmp(preset, "gfm_like")  == 0 ||
        strcmp(preset, "gfm-like2") == 0 ||
        strcmp(preset, "gfm_like2") == 0) {
        self->md.options.max_nesting = 20;
        self->md.options.html        = true;
        self->md.options.xhtml_out   = true;
        self->md.options.linkify     = true;
        mdit_str core_typographer[] = {
            MDIT_STR_LIT("replacements"),
            MDIT_STR_LIT("smartquotes"),
        };
        (void)mdit_ruler_disable(self->md.core.ruler, core_typographer,
                                 sizeof core_typographer / sizeof *core_typographer,
                                 true);
        if (self->md.linkifier == NULL) {
            mdit_md_set_linkifier(&self->md, mdit_linkifier_default());
        }
        const bool gfm2 = (strcmp(preset, "gfm-like2") == 0 ||
                           strcmp(preset, "gfm_like2") == 0);
        if (gfm2) {
            self->md.options.tasklists                  = true;
            self->md.options.tasklists_editable         = false;
            self->md.options.alerts                     = true;
            self->md.options.strikethrough_single_tilde = true;
        }
        return 0;
    }

    if (strcmp(preset, "zero") == 0) {
        /* The zero preset disables every block + inline + ruler2 rule
         * except a small "always-on" allowlist (paragraph, text,
         * normalize, block, inline). We snapshot each ruler's current
         * names, then disable everything not in the allowlist. */
        const char *block_keep[]   = { "paragraph" };
        const char *inline_keep[]  = { "text" };
        const char *inline2_keep[] = { "balance_pairs", "fragments_join" };
        const char *core_keep[]    = { "normalize", "block", "inline", "text_join" };
        const struct {
            mdit_ruler  *r;
            const char **keep;
            size_t       n_keep;
        } rulers[] = {
            { self->md.block.ruler,    block_keep,  1 },
            { self->md.inline_p.ruler, inline_keep, 1 },
            { self->md.inline_p.ruler2, inline2_keep, 2 },
            { self->md.core.ruler,     core_keep,   4 },
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
        "unknown preset %.200s (expected 'default', 'commonmark', 'zero', "
        "'gfm-like', or 'gfm-like2')",
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

/* Helper: copy bytes into the parser arena and return them as an
 * mdit_str view. Returns 0 on success, -1 on allocation failure. */
static int arena_dup_str(PyMarkdownIt *self, const char *s, size_t n,
                         mdit_str *out)
{
    if (n == 0 || s == NULL) {
        *out = (mdit_str){ "", 0 };
        return 0;
    }
    char *buf = (char *)mdit_arena_alloc_aligned(&self->arena, n, 1);
    if (buf == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memcpy(buf, s, n);
    out->data = buf;
    out->len  = n;
    return 0;
}

/* Bool-keyed options that map 1:1 to a `mdit_options` flag. */
static const struct option_bool_key {
    const char *key;
    size_t      offset;
} k_option_bool_keys[] = {
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

/* Build (or rebuild) ``self->options_dict`` so it mirrors the current
 * C ``mdit_options`` state. The dict identity is preserved when an
 * existing dict is present, matching upstream behaviour where
 * ``MarkdownIt.options`` is a single MutableMapping that user code can
 * cache and mutate. */
static int build_options_dict(PyMarkdownIt *self)
{
    PyObject *d = self->options_dict;
    if (d == NULL) {
        d = PyDict_New();
        if (d == NULL) return -1;
        self->options_dict = d;
    } else {
        PyDict_Clear(d);
    }

    PyObject *v;

    v = PyLong_FromLong(self->md.options.max_nesting);
    if (v == NULL) return -1;
    int rc = PyDict_SetItemString(d, "maxNesting", v);
    Py_DECREF(v);
    if (rc < 0) return -1;

    for (size_t i = 0; i < sizeof k_option_bool_keys / sizeof *k_option_bool_keys; ++i) {
        bool *slot = (bool *)((char *)&self->md.options
                              + k_option_bool_keys[i].offset);
        v = *slot ? Py_True : Py_False;
        if (PyDict_SetItemString(d, k_option_bool_keys[i].key, v) < 0)
            return -1;
    }

    v = PyUnicode_FromStringAndSize(self->md.options.lang_prefix.data,
                                    (Py_ssize_t)self->md.options.lang_prefix.len);
    if (v == NULL) return -1;
    rc = PyDict_SetItemString(d, "langPrefix", v);
    Py_DECREF(v);
    if (rc < 0) return -1;

    /* Build the upstream-shaped quotes string: 4 codepoints concatenated
     * (open-double, close-double, open-single, close-single). */
    char qbuf[64];
    size_t qlen = 0;
    for (int i = 0; i < 4; ++i) {
        size_t n = self->md.options.quotes[i].len;
        if (qlen + n > sizeof qbuf) { qlen = 0; break; }
        memcpy(qbuf + qlen, self->md.options.quotes[i].data, n);
        qlen += n;
    }
    v = PyUnicode_FromStringAndSize(qbuf, (Py_ssize_t)qlen);
    if (v == NULL) return -1;
    rc = PyDict_SetItemString(d, "quotes", v);
    Py_DECREF(v);
    if (rc < 0) return -1;

    /* `highlight` is preserved as-is so user code can stash a callable
     * alongside the rest of the options; the C engine itself doesn't
     * call back into Python yet, but tests that touch
     * ``options['highlight']`` shouldn't ``KeyError``. */
    if (PyDict_SetItemString(d, "highlight", Py_None) < 0) return -1;

    return 0;
}

/* Apply an `options` dict on top of the current preset state. Unknown
 * keys are ignored (upstream behaviour). Used both at `__init__` time
 * (to fold the user-supplied options dict into the preset baseline)
 * and at parse/render time (to re-sync the live ``options_dict`` into
 * the C ``mdit_options`` struct). */
static int apply_options_dict(PyMarkdownIt *self, PyObject *opts)
{
    if (opts == NULL || opts == Py_None) return 0;
    if (!PyDict_Check(opts) && !PyMapping_Check(opts)) {
        PyErr_SetString(PyExc_TypeError,
            "options must be a mapping or None");
        return -1;
    }

    /* Use PyMapping_GetItemString-like access so subclasses of dict
     * (e.g. upstream's OptionsDict, or any MutableMapping) work too. */
    for (size_t i = 0; i < sizeof k_option_bool_keys / sizeof *k_option_bool_keys; ++i) {
        PyObject *v = PyMapping_GetItemString(opts, k_option_bool_keys[i].key);
        if (v == NULL) {
            if (PyErr_Occurred()) PyErr_Clear();
            continue;
        }
        int parsed;
        int got = as_bool(v, &parsed);
        Py_DECREF(v);
        if (got < 0) return -1;
        if (got > 0) {
            bool *slot = (bool *)((char *)&self->md.options
                                  + k_option_bool_keys[i].offset);
            *slot = parsed ? true : false;
        }
    }

    PyObject *mn = PyMapping_GetItemString(opts, "maxNesting");
    if (mn != NULL) {
        if (mn != Py_None) {
            long val = PyLong_AsLong(mn);
            if (val == -1 && PyErr_Occurred()) {
                Py_DECREF(mn);
                return -1;
            }
            self->md.options.max_nesting = (int32_t)val;
        }
        Py_DECREF(mn);
    } else if (PyErr_Occurred()) {
        PyErr_Clear();
    }

    PyObject *lp = PyMapping_GetItemString(opts, "langPrefix");
    if (lp != NULL) {
        if (lp != Py_None) {
            if (!PyUnicode_Check(lp)) {
                Py_DECREF(lp);
                PyErr_SetString(PyExc_TypeError, "langPrefix must be a str");
                return -1;
            }
            Py_ssize_t n = 0;
            const char *s = PyUnicode_AsUTF8AndSize(lp, &n);
            if (s == NULL) { Py_DECREF(lp); return -1; }
            mdit_str dst;
            if (arena_dup_str(self, s, (size_t)n, &dst) < 0) {
                Py_DECREF(lp);
                return -1;
            }
            self->md.options.lang_prefix = dst;
        }
        Py_DECREF(lp);
    } else if (PyErr_Occurred()) {
        PyErr_Clear();
    }

    PyObject *quotes = PyMapping_GetItemString(opts, "quotes");
    if (quotes != NULL && quotes != Py_None) {
        /* upstream accepts either a 4-codepoint string ("“”‘’") or a
         * 4-element sequence of strings (one per quote slot). */
        if (PyUnicode_Check(quotes)) {
            Py_ssize_t cp_len = PyUnicode_GetLength(quotes);
            if (cp_len < 4) {
                Py_DECREF(quotes);
                PyErr_SetString(PyExc_ValueError,
                    "quotes string must have at least 4 codepoints");
                return -1;
            }
            for (int i = 0; i < 4; ++i) {
                PyObject *one = PySequence_GetSlice(quotes, i, i + 1);
                if (one == NULL) { Py_DECREF(quotes); return -1; }
                Py_ssize_t n = 0;
                const char *s = PyUnicode_AsUTF8AndSize(one, &n);
                if (s == NULL) {
                    Py_DECREF(one);
                    Py_DECREF(quotes);
                    return -1;
                }
                mdit_str dst;
                if (arena_dup_str(self, s, (size_t)n, &dst) < 0) {
                    Py_DECREF(one);
                    Py_DECREF(quotes);
                    return -1;
                }
                self->md.options.quotes[i] = dst;
                Py_DECREF(one);
            }
        } else if (PySequence_Check(quotes)) {
            if (PySequence_Length(quotes) < 4) {
                Py_DECREF(quotes);
                PyErr_SetString(PyExc_ValueError,
                    "quotes sequence must have at least 4 elements");
                return -1;
            }
            for (int i = 0; i < 4; ++i) {
                PyObject *one = PySequence_GetItem(quotes, i);
                if (one == NULL) { Py_DECREF(quotes); return -1; }
                if (!PyUnicode_Check(one)) {
                    Py_DECREF(one);
                    Py_DECREF(quotes);
                    PyErr_SetString(PyExc_TypeError,
                        "quotes elements must be strings");
                    return -1;
                }
                Py_ssize_t n = 0;
                const char *s = PyUnicode_AsUTF8AndSize(one, &n);
                if (s == NULL) {
                    Py_DECREF(one);
                    Py_DECREF(quotes);
                    return -1;
                }
                mdit_str dst;
                if (arena_dup_str(self, s, (size_t)n, &dst) < 0) {
                    Py_DECREF(one);
                    Py_DECREF(quotes);
                    return -1;
                }
                self->md.options.quotes[i] = dst;
                Py_DECREF(one);
            }
        } else {
            Py_DECREF(quotes);
            PyErr_SetString(PyExc_TypeError,
                "quotes must be a string or a sequence of 4 strings");
            return -1;
        }
        Py_DECREF(quotes);
    } else if (quotes == NULL) {
        if (PyErr_Occurred()) PyErr_Clear();
    } else {
        Py_DECREF(quotes);
    }

    /* Linkify needs a registered linkifier — install the default one
     * implicitly so callers don't have to. */
    if (self->md.options.linkify && self->md.linkifier == NULL) {
        mdit_md_set_linkifier(&self->md, mdit_linkifier_default());
    }

    return 0;
}

/* Re-sync the persistent ``options_dict`` into the C ``mdit_options``
 * struct. Called before each parse/render so user mutations like
 * ``md.options['typographer'] = True`` take effect. */
static int sync_options_from_dict(PyMarkdownIt *self)
{
    if (self->options_dict == NULL) return 0;
    return apply_options_dict(self, self->options_dict);
}

/* ------------------------------------------------------------------ */
/* tp_init / tp_dealloc                                                */
/* ------------------------------------------------------------------ */

static int PyMarkdownIt_init(PyMarkdownIt *self, PyObject *args, PyObject *kwargs)
{
    /* Upstream signature: MarkdownIt(config="commonmark", options_update=None).
     * We accept both ``config`` and ``preset`` as keyword aliases for
     * the first argument, and both ``options`` and ``options_update``
     * for the second, so test bodies copied verbatim from upstream
     * keep working. */
    const char *preset   = "commonmark";
    PyObject   *opts_obj = NULL;

    Py_ssize_t nargs = (args == NULL) ? 0 : PyTuple_GET_SIZE(args);
    if (nargs >= 1) {
        PyObject *first = PyTuple_GET_ITEM(args, 0);
        if (first != Py_None) {
            if (!PyUnicode_Check(first)) {
                PyErr_SetString(PyExc_TypeError,
                    "first positional argument (preset) must be a str");
                return -1;
            }
            preset = PyUnicode_AsUTF8(first);
            if (preset == NULL) return -1;
        }
    }
    if (nargs >= 2) {
        opts_obj = PyTuple_GET_ITEM(args, 1);
    }
    if (nargs > 2) {
        PyErr_Format(PyExc_TypeError,
            "MarkdownIt() takes at most 2 positional arguments (%zd given)",
            (Py_ssize_t)nargs);
        return -1;
    }
    if (kwargs != NULL && PyDict_Size(kwargs) > 0) {
        const char *cfg_keys[] = { "config", "preset", NULL };
        for (size_t i = 0; cfg_keys[i] != NULL; ++i) {
            PyObject *v = PyDict_GetItemString(kwargs, cfg_keys[i]);
            if (v != NULL && v != Py_None) {
                if (nargs >= 1) {
                    PyErr_Format(PyExc_TypeError,
                        "MarkdownIt() got multiple values for argument %s",
                        cfg_keys[i]);
                    return -1;
                }
                if (!PyUnicode_Check(v)) {
                    PyErr_Format(PyExc_TypeError,
                        "%s must be a str", cfg_keys[i]);
                    return -1;
                }
                preset = PyUnicode_AsUTF8(v);
                if (preset == NULL) return -1;
            }
            if (v != NULL) {
                PyDict_DelItemString(kwargs, cfg_keys[i]);
            }
        }
        const char *opt_keys[] = { "options", "options_update", NULL };
        for (size_t i = 0; opt_keys[i] != NULL; ++i) {
            PyObject *v = PyDict_GetItemString(kwargs, opt_keys[i]);
            if (v != NULL) {
                if (nargs >= 2 || opts_obj != NULL) {
                    PyErr_Format(PyExc_TypeError,
                        "MarkdownIt() got multiple values for options");
                    return -1;
                }
                opts_obj = v;
                PyDict_DelItemString(kwargs, opt_keys[i]);
            }
        }
        /* renderer_cls is silently ignored — the C engine has its own
         * built-in renderer, with no Python-callable rules yet. */
        PyDict_DelItemString(kwargs, "renderer_cls");
        if (PyErr_Occurred()) PyErr_Clear();
        if (PyDict_Size(kwargs) > 0) {
            PyObject *first_key = NULL;
            PyObject *_v = NULL;
            Py_ssize_t pos = 0;
            if (PyDict_Next(kwargs, &pos, &first_key, &_v) && first_key != NULL) {
                PyErr_Format(PyExc_TypeError,
                    "MarkdownIt() got an unexpected keyword argument '%U'",
                    first_key);
            } else {
                PyErr_SetString(PyExc_TypeError,
                    "MarkdownIt() got unexpected keyword arguments");
            }
            return -1;
        }
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

    /* Build a live ``options`` dict reflecting the post-preset +
     * post-overrides state. Subsequent mutations of this dict feed
     * back into the C engine via ``sync_options_from_dict`` on each
     * parse/render call. */
    if (build_options_dict(self) < 0) return -1;

    return 0;
}

static void PyMarkdownIt_dealloc(PyMarkdownIt *self)
{
    Py_CLEAR(self->render_callbacks);
    Py_CLEAR(self->options_dict);
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

static PyObject *PyMarkdownIt_render(PyMarkdownIt *self, PyObject *args,
                                     PyObject *kwargs)
{
    static char *kwlist[] = { "src", "env", NULL };
    PyObject *src_obj = NULL;
    PyObject *env_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwlist,
                                     &src_obj, &env_obj)) {
        return NULL;
    }

    if (!self->initialized) {
        PyErr_SetString(PyExc_RuntimeError, "MarkdownIt not initialized");
        return NULL;
    }

    Py_ssize_t   n = 0;
    const char  *s = PyUnicode_AsUTF8AndSize(src_obj, &n);
    if (s == NULL) return NULL;

    if (sync_options_from_dict(self) < 0) return NULL;

    /* Reset the arena between renders so memory doesn't grow without
     * bound across many calls on the same instance. mdit_md_init kept
     * pointers into the arena (renderer slots, ruler entries), so we
     * must NOT reset — instead we accept that long-lived instances
     * grow proportionally to total parse activity. A future slice
     * will introduce a per-render scratch arena. */
    mdit_buf out;
    mdit_buf_init(&out);
    mdit_str src = { s, (size_t)n };
    mdit_env c_env;
    mdit_env_init(&c_env, self->md.arena);
    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, self->md.arena);
    if (!mdit_md_parse(&self->md, src, &c_env, &tokens)) {
        mdit_vec_token_destroy(&tokens);
        mdit_buf_destroy(&out);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "mdit_md_parse failed");
        }
        return NULL;
    }
    mdit_renderer_options ropts;
    ropts.xhtmlOut           = self->md.options.xhtml_out;
    ropts.breaks             = self->md.options.breaks;
    ropts.tasklists_editable = self->md.options.tasklists_editable;
    ropts.langPrefix         = self->md.options.lang_prefix;
    PyRenderContext render_ctx = { self, env_obj };
    bool ok = mdit_renderer_render(self->md.renderer, tokens.data, tokens.len,
                                   &ropts, &render_ctx, &out);
    mdit_vec_token_destroy(&tokens);
    if (!ok) {
        mdit_buf_destroy(&out);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "mdit_renderer_render failed");
        }
        return NULL;
    }
    PyObject *result = PyUnicode_DecodeUTF8(
        (out.data ? out.data : ""), (Py_ssize_t)out.len, "strict");
    mdit_buf_destroy(&out);
    return result;
}

/* ------------------------------------------------------------------ */
/* parse()                                                             */
/* ------------------------------------------------------------------ */

static PyObject *PyMarkdownIt_parse(PyMarkdownIt *self, PyObject *args,
                                    PyObject *kwargs)
{
    static char *kwlist[] = { "src", "env", NULL };
    PyObject *src_obj = NULL;
    PyObject *env_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwlist,
                                     &src_obj, &env_obj)) {
        return NULL;
    }
    (void)env_obj; /* env support lands with the fuller plugin surface. */

    if (!self->initialized) {
        PyErr_SetString(PyExc_RuntimeError, "MarkdownIt not initialized");
        return NULL;
    }

    Py_ssize_t n = 0;
    const char *s = PyUnicode_AsUTF8AndSize(src_obj, &n);
    if (s == NULL) return NULL;

    if (sync_options_from_dict(self) < 0) return NULL;

    mdit_vec_token tokens;
    mdit_vec_token_init(&tokens, &self->arena);
    mdit_str src = { s, (size_t)n };
    if (!mdit_md_parse(&self->md, src, NULL, &tokens)) {
        mdit_vec_token_destroy(&tokens);
        PyErr_SetString(PyExc_RuntimeError, "mdit_md_parse failed");
        return NULL;
    }

    PyObject *list = PyList_New((Py_ssize_t)tokens.len);
    if (list == NULL) {
        mdit_vec_token_destroy(&tokens);
        return NULL;
    }
    for (size_t i = 0; i < tokens.len; ++i) {
        PyObject *tok = PyToken_from_mdit(&tokens.data[i]);
        if (tok == NULL) {
            Py_DECREF(list);
            mdit_vec_token_destroy(&tokens);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, tok);
    }

    mdit_vec_token_destroy(&tokens);
    return list;
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

static mdit_ruler *select_ruler(PyMarkdownIt *self, const char *chain)
{
    if (strcmp(chain, "core") == 0) return self->md.core.ruler;
    if (strcmp(chain, "block") == 0) return self->md.block.ruler;
    if (strcmp(chain, "inline") == 0) return self->md.inline_p.ruler;
    if (strcmp(chain, "inline2") == 0) return self->md.inline_p.ruler2;
    return NULL;
}

static PyObject *str_array_to_pylist(const mdit_str *names, size_t n)
{
    PyObject *list = PyList_New((Py_ssize_t)n);
    if (list == NULL) return NULL;
    for (size_t i = 0; i < n; ++i) {
        PyObject *s = PyUnicode_FromStringAndSize(
            names[i].data, (Py_ssize_t)names[i].len);
        if (s == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, s);
    }
    return list;
}

static PyObject *PyMarkdownIt_ruler_get_all(PyMarkdownIt *self, PyObject *arg)
{
    const char *chain = PyUnicode_AsUTF8(arg);
    if (chain == NULL) return NULL;
    mdit_ruler *r = select_ruler(self, chain);
    if (r == NULL) {
        PyErr_Format(PyExc_KeyError, "unknown ruler chain: %s", chain);
        return NULL;
    }
    size_t n = 0;
    const mdit_str *names = mdit_ruler_all_rule_names(r, &n);
    return str_array_to_pylist(names, n);
}

static PyObject *PyMarkdownIt_ruler_get_active(PyMarkdownIt *self, PyObject *arg)
{
    const char *chain = PyUnicode_AsUTF8(arg);
    if (chain == NULL) return NULL;
    mdit_ruler *r = select_ruler(self, chain);
    if (r == NULL) {
        PyErr_Format(PyExc_KeyError, "unknown ruler chain: %s", chain);
        return NULL;
    }
    size_t n = 0;
    const mdit_str *names = mdit_ruler_active_rule_names(r, &n);
    return str_array_to_pylist(names, n);
}

static PyObject *ruler_toggle_chain(PyMarkdownIt *self, PyObject *args,
                                    PyObject *kwargs, int mode)
{
    static char *kwlist[] = { "chain", "names", "ignoreInvalid", NULL };
    const char *chain = NULL;
    PyObject *names_obj = NULL;
    int ignore = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|p", kwlist,
                                     &chain, &names_obj, &ignore)) {
        return NULL;
    }
    mdit_ruler *r = select_ruler(self, chain);
    if (r == NULL) {
        PyErr_Format(PyExc_KeyError, "unknown ruler chain: %s", chain);
        return NULL;
    }

    mdit_str *names = NULL;
    size_t n = 0;
    PyObject *holder = NULL;
    if (collect_names(names_obj, &names, &n, &holder) < 0) return NULL;

    PyObject *found = PyList_New(0);
    if (found == NULL) {
        PyMem_Free(names);
        Py_DECREF(holder);
        return NULL;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!mdit_ruler_has(r, names[i])) {
            if (!ignore) {
                Py_DECREF(found);
                PyMem_Free(names);
                Py_DECREF(holder);
                PyErr_Format(PyExc_KeyError,
                    "Rules manager: invalid rule name %.*s",
                    (int)names[i].len, names[i].data);
                return NULL;
            }
            continue;
        }
        PyObject *s = PyUnicode_FromStringAndSize(
            names[i].data, (Py_ssize_t)names[i].len);
        if (s == NULL || PyList_Append(found, s) < 0) {
            Py_XDECREF(s);
            Py_DECREF(found);
            PyMem_Free(names);
            Py_DECREF(holder);
            return NULL;
        }
        Py_DECREF(s);
    }

    int rc;
    if (mode == 0) {
        rc = mdit_ruler_disable(r, names, n, true);
    } else if (mode == 1) {
        rc = mdit_ruler_enable(r, names, n, true);
    } else {
        rc = mdit_ruler_enable_only(r, names, n, true);
    }
    PyMem_Free(names);
    Py_DECREF(holder);
    if (rc < 0) {
        Py_DECREF(found);
        PyErr_SetString(PyExc_RuntimeError, "ruler mutation failed");
        return NULL;
    }
    return found;
}

static PyObject *PyMarkdownIt_ruler_enable(PyMarkdownIt *self, PyObject *args,
                                           PyObject *kwargs)
{
    return ruler_toggle_chain(self, args, kwargs, 1);
}

static PyObject *PyMarkdownIt_ruler_disable(PyMarkdownIt *self, PyObject *args,
                                            PyObject *kwargs)
{
    return ruler_toggle_chain(self, args, kwargs, 0);
}

static PyObject *PyMarkdownIt_ruler_enable_only(PyMarkdownIt *self, PyObject *args,
                                                PyObject *kwargs)
{
    return ruler_toggle_chain(self, args, kwargs, 2);
}

static PyObject *PyMarkdownIt_add_render_rule(PyMarkdownIt *self, PyObject *args)
{
    const char *name = NULL;
    Py_ssize_t name_len = 0;
    PyObject *callback = NULL;
    if (!PyArg_ParseTuple(args, "s#O", &name, &name_len, &callback)) {
        return NULL;
    }
    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "render rule callback must be callable");
        return NULL;
    }
    if (self->render_callbacks == NULL) {
        self->render_callbacks = PyDict_New();
        if (self->render_callbacks == NULL) return NULL;
    }
    PyObject *key = PyUnicode_FromStringAndSize(name, name_len);
    if (key == NULL) return NULL;
    if (PyDict_SetItem(self->render_callbacks, key, callback) < 0) {
        Py_DECREF(key);
        return NULL;
    }
    Py_DECREF(key);
    mdit_str token_type;
    if (arena_dup_str(self, name, (size_t)name_len, &token_type) < 0) {
        return NULL;
    }
    if (!mdit_renderer_add_rule(self->md.renderer, token_type,
                                py_render_rule_bridge)) {
        PyErr_SetString(PyExc_RuntimeError, "mdit_renderer_add_rule failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* ------------------------------------------------------------------ */
/* options getter / setter                                             */
/* ------------------------------------------------------------------ */

static PyObject *PyMarkdownIt_options_get(PyMarkdownIt *self, void *closure)
{
    (void)closure;
    if (self->options_dict == NULL) {
        if (build_options_dict(self) < 0) return NULL;
    }
    Py_INCREF(self->options_dict);
    return self->options_dict;
}

static int PyMarkdownIt_options_set(PyMarkdownIt *self, PyObject *value,
                                    void *closure)
{
    (void)closure;
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "options cannot be deleted");
        return -1;
    }
    /* Fold user keys into the C state first, so build_options_dict
     * picks them up when it rebuilds the canonical dict from
     * mdit_options. Then preserve any extra keys the caller supplied
     * (e.g. ``highlight``, ``store_labels``) by replaying `value` over
     * the rebuilt dict. */
    if (apply_options_dict(self, value) < 0) return -1;
    if (build_options_dict(self) < 0) return -1;
    if (PyMapping_Check(value)) {
        if (PyDict_Check(value)) {
            if (PyDict_Update(self->options_dict, value) < 0) return -1;
        } else {
            /* Generic mapping: copy each item explicitly via
             * PyMapping_Items + dict-set. */
            PyObject *items = PyMapping_Items(value);
            if (items == NULL) return -1;
            Py_ssize_t n = PySequence_Length(items);
            for (Py_ssize_t i = 0; i < n; ++i) {
                PyObject *pair = PySequence_GetItem(items, i);
                if (pair == NULL) { Py_DECREF(items); return -1; }
                PyObject *k = PySequence_GetItem(pair, 0);
                PyObject *v = PySequence_GetItem(pair, 1);
                Py_DECREF(pair);
                if (k == NULL || v == NULL) {
                    Py_XDECREF(k); Py_XDECREF(v);
                    Py_DECREF(items);
                    return -1;
                }
                int rc = PyDict_SetItem(self->options_dict, k, v);
                Py_DECREF(k); Py_DECREF(v);
                if (rc < 0) { Py_DECREF(items); return -1; }
            }
            Py_DECREF(items);
        }
    }
    return 0;
}

static PyGetSetDef PyMarkdownIt_getsetters[] = {
    { "options",
      (getter)PyMarkdownIt_options_get,
      (setter)PyMarkdownIt_options_set,
      "Parser options as a live dict (mutations propagate on next "
      "parse/render). Mirrors upstream ``MarkdownIt.options``.",
      NULL },
    { NULL }
};

static PyMethodDef PyMarkdownIt_methods[] = {
    { "parse",   (PyCFunction)PyMarkdownIt_parse,
      METH_VARARGS | METH_KEYWORDS,
      "parse(src, env=None) -> list[Token]: parse Markdown to tokens." },
    { "render",  (PyCFunction)PyMarkdownIt_render,
      METH_VARARGS | METH_KEYWORDS,
      "render(src) -> str: parse + render Markdown to HTML." },
    { "enable",  (PyCFunction)PyMarkdownIt_enable,  METH_VARARGS,
      "enable(names, ignoreInvalid=False) -> self: enable rule(s)." },
    { "disable", (PyCFunction)PyMarkdownIt_disable, METH_VARARGS,
      "disable(names, ignoreInvalid=False) -> self: disable rule(s)." },
    { "_ruler_get_all", (PyCFunction)PyMarkdownIt_ruler_get_all, METH_O,
      "_ruler_get_all(chain) -> list[str]." },
    { "_ruler_get_active", (PyCFunction)PyMarkdownIt_ruler_get_active, METH_O,
      "_ruler_get_active(chain) -> list[str]." },
    { "_ruler_enable", (PyCFunction)PyMarkdownIt_ruler_enable,
      METH_VARARGS | METH_KEYWORDS,
      "_ruler_enable(chain, names, ignoreInvalid=False) -> list[str]." },
    { "_ruler_disable", (PyCFunction)PyMarkdownIt_ruler_disable,
      METH_VARARGS | METH_KEYWORDS,
      "_ruler_disable(chain, names, ignoreInvalid=False) -> list[str]." },
    { "_ruler_enable_only", (PyCFunction)PyMarkdownIt_ruler_enable_only,
      METH_VARARGS | METH_KEYWORDS,
      "_ruler_enable_only(chain, names, ignoreInvalid=False) -> list[str]." },
    { "_add_render_rule", (PyCFunction)PyMarkdownIt_add_render_rule,
      METH_VARARGS,
      "_add_render_rule(name, callback) -> None." },
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
    if (PyType_Ready(&PyToken_Type) < 0) return NULL;
    if (PyType_Ready(&PyMarkdownIt_Type) < 0) return NULL;

    PyObject *m = PyModule_Create(&mdit_c_moduledef);
    if (m == NULL) return NULL;

    Py_INCREF(&PyToken_Type);
    if (PyModule_AddObject(m, "Token", (PyObject *)&PyToken_Type) < 0) {
        Py_DECREF(&PyToken_Type);
        Py_DECREF(m);
        return NULL;
    }

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
