lila_module(lila_editor UI
    CodeEditor.cpp CodeEditor.h
    CompletionPopup.cpp CompletionPopup.h
    EditorPalette.h
    EditorTypes.h
    FindBar.cpp FindBar.h
    Interpreter.cpp Interpreter.h
    LspClient.cpp LspClient.h
    OutputView.cpp OutputView.h
    PythonHighlighter.cpp PythonHighlighter.h
    Ruff.cpp Ruff.h
    ScriptEditor.cpp ScriptEditor.h
    ScriptStorage.cpp ScriptStorage.h
    TextUtil.cpp TextUtil.h
    Tools.cpp Tools.h
    VimBinding.cpp VimBinding.h
    VimEngine.cpp VimEngine.h)
set_target_properties(lila_editor PROPERTIES AUTOMOC ON)
target_link_libraries(lila_editor PUBLIC lila_python Qt6::Widgets nlohmann_json::nlohmann_json PRIVATE Python::Python)

cmake_path(GET Python_EXECUTABLE PARENT_PATH LILA_EDITOR_PYTHON_HOME)
target_compile_definitions(lila_editor PRIVATE LILA_PYTHON_HOME="${LILA_EDITOR_PYTHON_HOME}")
