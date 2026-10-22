# Layout files and commands, without Qt, so the tests and batch mode can use them.
add_library(lila_layouts STATIC Layouts.cpp Layouts.h)
target_link_libraries(lila_layouts PUBLIC lila_cmds)

add_library(lila_ui STATIC
    Actions.cpp
    AdjustLastOp.cpp AdjustLastOp.h
    FlagFields.cpp FlagFields.h
    OptionBox.cpp OptionBox.h
    AgentCapture.cpp AgentCapture.h
    AgentSetup.cpp AgentSetup.h
    Animation.cpp Animation.h
    ApiReference.cpp ApiReference.h
    AttributeEditor.cpp AttributeEditor.h
    AutoBevelUi.cpp AutoBevelUi.h
    Camera.cpp
    ChannelBox.cpp ChannelBox.h
    DisplayLayersPanel.cpp DisplayLayersPanel.h
    Editors.cpp Editors.h
    Environments.cpp Environments.h
    GraphEditor.cpp GraphEditor.h
    HotkeyBindings.cpp HotkeyBindings.h
    HotkeyCheatSheet.cpp HotkeyCheatSheet.h
    HotkeyEditor.cpp HotkeyEditor.h
    Hotkeys.cpp Hotkeys.h
    Hotbox.cpp Hotbox.h HotboxZone.h HotkeyChord.h
    Icons.cpp Icons.h
    ImagePlaneUi.cpp ImagePlaneUi.h
    LayoutsUi.cpp LayoutsUi.h
    MarkingMenu.cpp MarkingMenu.h
    MarkingMenuEditor.cpp MarkingMenuEditor.h
    MainWindow.cpp MainWindow.h
    SpinScrub.cpp SpinScrub.h
    ValueReset.h
    Manipulator.cpp Manipulator.h
    ExtrudeHandles.cpp ExtrudeHandles.h
    MaterialDraw.cpp MaterialDraw.h
    MaterialEditor.cpp MaterialEditor.h
    MeasureTool.cpp MeasureTool.h
    MeshCache.cpp MeshCache.h
    ModelingToolkit.cpp ModelingToolkit.h
    SoftSelectPanel.cpp SoftSelectPanel.h
    ModifierStack.cpp ModifierStack.h
    NodeEditor.cpp NodeEditor.h
    Overlay.cpp Overlay.h
    CursorBadges.cpp CursorBadges.h
    Panels.cpp Panels.h
    QuadDrawTool.cpp QuadDrawTool.h
    PythonBridge.cpp PythonBridge.h
    RigViewport.cpp RigViewport.h WeightPaintTool.cpp WeightPaintTool.h
    DeformUi.cpp DeformUi.h
    SculptPanel.cpp SculptPanel.h
    SculptTool.cpp SculptTool.h
    Tools.cpp Tools.h
    Outliner.cpp Outliner.h
    PreferencesDialog.cpp PreferencesDialog.h
    ProgressOverlay.cpp ProgressOverlay.h
    ReferenceEditor.cpp ReferenceEditor.h
    RenderSettingsDialog.cpp RenderSettingsDialog.h
    ScriptPanel.cpp ScriptPanel.h
    Shelf.cpp Shelf.h
    StatusOutput.cpp StatusOutput.h OutputTail.h
    SceneDialogs.cpp SceneDialogs.h
    SourceControlPanel.cpp SourceControlPanel.h
    TearOff.cpp TearOff.h TearOffLogic.h
    Terminal.cpp Terminal.h
    TextureCache.cpp TextureCache.h
    Theme.cpp
    TimeSlider.cpp TimeSlider.h
    UiTools.cpp
    UndoHistory.cpp UndoHistory.h
    UvEditor.cpp UvEditor.h
    UvLive.cpp UvLive.h
    UvPackPanel.cpp UvPackPanel.h
    FlowLayout.cpp FlowLayout.h
    DockSidebar.cpp DockSidebar.h
    ViewportHooks.cpp ViewportHooks.h
    ViewportDisplay.cpp ViewportDisplay.h
    ViewportHud.cpp ViewportHud.h
    ViewportLayout.cpp ViewportLayout.h
    ViewportToolbar.cpp ViewportToolbar.h
    VertexPaintTool.cpp VertexPaintTool.h
    ViewportTool.cpp ViewportTool.h
    ViewportWidget.cpp ViewportWidget.h)

set_target_properties(lila_ui PROPERTIES AUTOMOC ON)
target_link_libraries(lila_ui PUBLIC lila_scene lila_io lila_python lila_mcp lila_render lila_editor lila_vcs Qt6::Widgets)
target_link_libraries(lila_ui PUBLIC lila_term)
target_link_libraries(lila_ui PUBLIC lila_layouts)
target_link_libraries(lila_ui PUBLIC lila_uv_cmds)
target_link_libraries(lila_ui PUBLIC lila_gn)
target_link_libraries(lila_ui PUBLIC lila_retopo_cmds)
target_link_libraries(lila_ui PUBLIC lila_sculpt_cmds)
target_link_libraries(lila_ui PUBLIC lila_vcolor)
target_link_libraries(lila_ui PRIVATE Python::Python Qt6::Svg)
