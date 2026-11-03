"""The bundled PySide6: it imports, matches the Qt Lila runs on, and binds to Lila's Qt DLLs rather than its own.

Run with: lilabatch.exe --batch X:/Lila/python/tests/test_pyside.py
"""

import ctypes
import os


def loaded_module_dir(name):
    """Folder of a DLL already loaded in this process; None names the exe."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p
    kernel32.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32]
    handle = kernel32.GetModuleHandleW(name)
    assert handle, "%s is not loaded" % name
    buf = ctypes.create_unicode_buffer(1024)
    kernel32.GetModuleFileNameW(handle, buf, len(buf))
    return os.path.normcase(os.path.dirname(buf.value))


def test_bundled_copy_wins():
    import PySide6

    site = os.path.normcase(os.path.dirname(os.path.dirname(PySide6.__file__)))
    assert os.path.basename(site) == "python-site", PySide6.__file__


def test_version_matches_qt():
    import PySide6
    from PySide6 import QtCore

    assert PySide6.__version__ == QtCore.qVersion(), (PySide6.__version__, QtCore.qVersion())


MODULES = ("Core", "Gui", "Widgets", "Network", "Svg", "Concurrent", "Xml", "Sql", "PrintSupport", "OpenGL",
           "OpenGLWidgets", "UiTools")
# QtConcurrent's bindings are templates over QtCore, so it has no DLL of its own.
QT_DLLS = [m for m in MODULES if m != "Concurrent"]


def app():
    from PySide6 import QtWidgets

    return QtWidgets.QApplication.instance() or QtWidgets.QApplication(["lila", "-platform", "offscreen"])


def test_every_module_imports():
    import importlib

    for module in MODULES:
        importlib.import_module("PySide6.Qt" + module)


def test_binds_lila_qt():
    import PySide6

    exe_dir = loaded_module_dir(None)
    pyside_dir = os.path.normcase(os.path.dirname(PySide6.__file__))
    for module in QT_DLLS:
        name = "Qt6%s.dll" % module
        assert loaded_module_dir(name) == exe_dir, (name, loaded_module_dir(name))
        assert not os.path.exists(os.path.join(pyside_dir, name)), name


def test_network_ssl():
    from PySide6 import QtNetwork

    # TLS backends are plugins, found beside the exe once an application exists.
    app()
    assert "openssl" in QtNetwork.QSslSocket.availableBackends(), QtNetwork.QSslSocket.availableBackends()
    assert QtNetwork.QSslSocket.supportsSsl()
    assert QtNetwork.QSslSocket.sslLibraryVersionString().startswith("OpenSSL")
    assert QtNetwork.QDtls(QtNetwork.QSslSocket.SslMode.SslClientMode).sslMode() == QtNetwork.QSslSocket.SslMode.SslClientMode


def test_widgets_and_shiboken():
    import shiboken6
    from PySide6 import QtWidgets

    app()
    window = QtWidgets.QMainWindow()
    label = QtWidgets.QLabel("PySide")
    window.setCentralWidget(label)
    assert window.centralWidget() is label
    pointer = shiboken6.getCppPointer(window)[0]
    assert shiboken6.wrapInstance(pointer, QtWidgets.QMainWindow) is window


def test_ui_loader():
    from PySide6 import QtCore, QtUiTools, QtWidgets

    app()
    ui = (b'<ui version="4.0"><class>Form</class><widget class="QWidget" name="Form"><layout class="QVBoxLayout">'
          b'<item><widget class="QPushButton" name="go"><property name="text"><string>Go</string></property></widget>'
          b'</item></layout></widget></ui>')
    buffer = QtCore.QBuffer()
    buffer.setData(ui)
    buffer.open(QtCore.QIODevice.OpenModeFlag.ReadOnly)
    form = QtUiTools.QUiLoader().load(buffer)
    assert form is not None
    assert form.findChild(QtWidgets.QPushButton, "go").text() == "Go"


def test_sqlite():
    from PySide6 import QtSql

    assert "QSQLITE" in QtSql.QSqlDatabase.drivers(), QtSql.QSqlDatabase.drivers()
    db = QtSql.QSqlDatabase.addDatabase("QSQLITE", "pyside_test")
    db.setDatabaseName(":memory:")
    assert db.open(), db.lastError().text()
    query = QtSql.QSqlQuery(db)
    assert query.exec("select 6 * 7") and query.next()
    assert query.value(0) == 42
    del query
    db.close()
    del db
    QtSql.QSqlDatabase.removeDatabase("pyside_test")


def test_xml_print_opengl():
    from PySide6 import QtOpenGLWidgets, QtPrintSupport, QtXml

    app()
    doc = QtXml.QDomDocument()
    assert doc.setContent("<shot name='010'/>")
    assert doc.documentElement().attribute("name") == "010"
    printer = QtPrintSupport.QPrinter()
    printer.setOutputFormat(QtPrintSupport.QPrinter.OutputFormat.PdfFormat)
    assert printer.outputFormat() == QtPrintSupport.QPrinter.OutputFormat.PdfFormat
    assert QtOpenGLWidgets.QOpenGLWidget.staticMetaObject.className() == "QOpenGLWidget"


def test_qt_py_shim():
    import Qt

    assert Qt.__binding__ == "PySide6", Qt.__binding__
    assert Qt.QtWidgets.QMainWindow.__module__.startswith("PySide6")


def main():
    tests = [(k, v) for k, v in list(globals().items()) if k.startswith("test_") and callable(v)]
    for name, fn in tests:
        fn()
        print("ok  " + name)
    print("pyside: %d tests passed" % len(tests))


main()
