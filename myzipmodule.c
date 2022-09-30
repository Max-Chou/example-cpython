#include <zlib.h>
#include <Python.h>
#include "pythread.h"


/* Initial buffer size. */
#define DEF_BUF_SIZE (16*1024)


static PyObject *ZlibError;


static void*
PyZlib_Malloc(voidpf ctx, uInt items, uInt size)
{
    if (size != 0 && items > (size_t)PY_SSIZE_T_MAX / size)
        return NULL;
    /* PyMem_Malloc() cannot be used: the GIL is not held when
       inflate() and deflate() are called */
    return PyMem_RawMalloc((size_t)items * (size_t)size);
}


static void
PyZlib_Free(voidpf ctx, void *ptr)
{
    PyMem_RawFree(ptr);
}

static void
arrange_input_buffer(z_stream *zst, Py_ssize_t *remains)
{
    zst->avail_in = (uInt)Py_MIN((size_t)*remains, UINT_MAX);
    *remains -= zst->avail_in;
}

static Py_ssize_t
arrange_output_buffer_with_maximum(z_stream *zst, PyObject **buffer,
                                   Py_ssize_t length,
                                   Py_ssize_t max_length)
{
    Py_ssize_t occupied;

    if (*buffer == NULL) {
        if (!(*buffer = PyBytes_FromStringAndSize(NULL, length)))
            return -1;
        occupied = 0;
    }
    else {
        occupied = zst->next_out - (Byte *)PyBytes_AS_STRING(*buffer);

        if (length == occupied) {
            Py_ssize_t new_length;
            assert(length <= max_length);
            /* can not scale the buffer over max_length */
            if (length == max_length)
                return -2;
            if (length <= (max_length >> 1))
                new_length = length << 1;
            else
                new_length = max_length;
            if (_PyBytes_Resize(buffer, new_length) < 0)
                return -1;
            length = new_length;
        }
    }

    zst->avail_out = (uInt)Py_MIN((size_t)(length - occupied), UINT_MAX);
    zst->next_out = (Byte *)PyBytes_AS_STRING(*buffer) + occupied;

    return length;
}

static Py_ssize_t
arrange_output_buffer(z_stream *zst, PyObject **buffer, Py_ssize_t length)
{
    Py_ssize_t ret;

    ret = arrange_output_buffer_with_maximum(zst, buffer, length,
                                             PY_SSIZE_T_MAX);
    if (ret == -2)
        PyErr_NoMemory();

    return ret;
}


static void
zlib_error(z_stream zst, int err, const char *msg)
{
    const char *zmsg = Z_NULL;
    /* In case of a version mismatch, zst.msg won't be initialized.
       Check for this case first, before looking at zst.msg. */
    if (err == Z_VERSION_ERROR)
        zmsg = "library version mismatch";
    if (zmsg == Z_NULL)
        zmsg = zst.msg;
    if (zmsg == Z_NULL) {
        switch (err) {
        case Z_BUF_ERROR:
            zmsg = "incomplete or truncated stream";
            break;
        case Z_STREAM_ERROR:
            zmsg = "inconsistent stream state";
            break;
        case Z_DATA_ERROR:
            zmsg = "invalid input data";
            break;
        }
    }
    if (zmsg == Z_NULL)
        PyErr_Format(ZlibError, "Error %d %s", err, msg);
    else
        PyErr_Format(ZlibError, "Error %d %s: %.200s", err, msg, zmsg);
}


static PyObject *myzip_compress_impl(PyObject *module, Py_buffer *data, int level) {
    PyObject *RetVal = NULL;
    Byte *ibuf;
    Py_ssize_t ibuflen, obuflen = DEF_BUF_SIZE;
    int err, flush;
    z_stream zst;

    ibuf = data->buf;
    ibuflen = data->len;

    zst.opaque = NULL;
    zst.zalloc = PyZlib_Malloc;
    zst.zfree = PyZlib_Free;
    zst.next_in = ibuf;
    err = deflateInit(&zst, level);


    switch (err) {
    case Z_OK:
        break;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Out of memory while compressing data");
        goto error;
    case Z_STREAM_ERROR:
        PyErr_SetString(ZlibError, "Bad compression level");
        goto error;
    default:
        deflateEnd(&zst);
        zlib_error(zst, err, "while compressing data");
        goto error;
    }

    do {
        arrange_input_buffer(&zst, &ibuflen);
        flush = ibuflen == 0 ? Z_FINISH : Z_NO_FLUSH;

        do {
            obuflen = arrange_output_buffer(&zst, &RetVal, obuflen);
            if (obuflen < 0) {
                deflateEnd(&zst);
                goto error;
            }

            
            err = deflate(&zst, flush);
            

            if (err == Z_STREAM_ERROR) {
                deflateEnd(&zst);
                zlib_error(zst, err, "while compressing data");
                goto error;
            }

        } while (zst.avail_out == 0);
        assert(zst.avail_in == 0);

    } while (flush != Z_FINISH);
    assert(err == Z_STREAM_END);

    err = deflateEnd(&zst);
    if (err == Z_OK) {
        if (_PyBytes_Resize(&RetVal, zst.next_out -
                            (Byte *)PyBytes_AS_STRING(RetVal)) < 0)
            goto error;

        return RetVal;
    }
    else
        zlib_error(zst, err, "while finishing compression");
    
error:
    Py_XDECREF(RetVal);
    return NULL;
}


static PyObject *
myzip_compress(PyObject *module, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"", "level", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "compress", 0};
    PyObject *argsbuf[2];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 1;
    Py_buffer data = {NULL, NULL};
    int level = Z_DEFAULT_COMPRESSION;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser, 1, 2, 0, argsbuf);
    if (!args) {
        goto exit;
    }
    if (PyObject_GetBuffer(args[0], &data, PyBUF_SIMPLE) != 0) {
        goto exit;
    }
    if (!PyBuffer_IsContiguous(&data, 'C')) {
        _PyArg_BadArgument("compress", "argument 1", "contiguous buffer", args[0]);
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (PyFloat_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError,
                        "integer argument expected, got float" );
        goto exit;
    }
    level = _PyLong_AsInt(args[1]);
    if (level == -1 && PyErr_Occurred()) {
        goto exit;
    }
skip_optional_pos:
    return_value = myzip_compress_impl(module, &data, level);

exit:
    /* Cleanup for data */
    if (data.obj) {
       PyBuffer_Release(&data);
    }

    return return_value;
}




static PyMethodDef myzip_methods[] = {
    {"compress", (PyCFunction)(void(*)(void))myzip_compress, METH_FASTCALL|METH_KEYWORDS, "compress"},
    {NULL, NULL}
};


static struct PyModuleDef myzipmodule = {
    PyModuleDef_HEAD_INIT,
    "myzip",
    "Using zlib in python",
    -1,
    myzip_methods,
    NULL,
    NULL,
    NULL,
    NULL
};


PyMODINIT_FUNC
PyInit_myzip(void)
{
    PyObject *m;
    m = PyModule_Create(&myzipmodule);
    if (m == NULL) {
        return NULL;
    }

    ZlibError = PyErr_NewException("zlib.error", NULL, NULL);
    if (ZlibError != NULL) {
        Py_INCREF(ZlibError);
        PyModule_AddObject(m, "error", ZlibError);
    }

    PyModule_AddIntMacro(m, DEF_BUF_SIZE);

    PyModule_AddIntMacro(m, Z_NO_COMPRESSION);
    PyModule_AddIntMacro(m, Z_BEST_SPEED);
    PyModule_AddIntMacro(m, Z_BEST_COMPRESSION);
    PyModule_AddIntMacro(m, Z_DEFAULT_COMPRESSION);
    // compression strategies
    PyModule_AddIntMacro(m, Z_FILTERED);
    PyModule_AddIntMacro(m, Z_HUFFMAN_ONLY);
#ifdef Z_RLE // 1.2.0.1
    PyModule_AddIntMacro(m, Z_RLE);
#endif
#ifdef Z_FIXED // 1.2.2.2
    PyModule_AddIntMacro(m, Z_FIXED);
#endif
    PyModule_AddIntMacro(m, Z_DEFAULT_STRATEGY);
    // allowed flush values
    PyModule_AddIntMacro(m, Z_NO_FLUSH);
    PyModule_AddIntMacro(m, Z_PARTIAL_FLUSH);
    PyModule_AddIntMacro(m, Z_SYNC_FLUSH);
    PyModule_AddIntMacro(m, Z_FULL_FLUSH);
    PyModule_AddIntMacro(m, Z_FINISH);
#ifdef Z_BLOCK // 1.2.0.5 for inflate, 1.2.3.4 for deflate
    PyModule_AddIntMacro(m, Z_BLOCK);
#endif
#ifdef Z_TREES // 1.2.3.4, only for inflate
    PyModule_AddIntMacro(m, Z_TREES);
#endif

    return m;
}
