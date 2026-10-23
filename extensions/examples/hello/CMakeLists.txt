# Sample C++ extension, built as <build>/bin/<config>/extensions/hello.lilaext, which Lila searches.
lila_add_extension(lila_hello_extension ID hello MANIFEST extension.json SOURCES Hello.cpp)
