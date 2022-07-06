# example-cpython
How to use C library from python

## C Code

### Method
```c
static PyMethodDef FputsMethods[] = {
    {"fputs", method_fputs, METH_VARARGS, "Python interface for fputs C Library function"},
    {NULL, NULL, 0, NULL}
};
```
### Module
```c
static struct PyModuleDef fputsmodule = {
    PyModuleDef_HEAD_INIT,
    "fputs",
    "Python interface for the fputs C Library function",
    -1,
    FputsMethods
};
```


## Install

Using `distutils.core` and `setup.py`


```bash
python3 setup.py install
```
