from distutils.core import setup, Extension

def main():
    setup(
        name="fputs",
        version="0.0.1",
        description="Python Interface for the fputs C Library function",
        author="test",
        author_email="test@example.com",
        ext_modules=[
            Extension("fputs", ["fputsmodule.c"])
        ]
    )
    

if __name__ == "__main__":
    main()
