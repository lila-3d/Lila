# Layout and theme files and commands, without Qt, so the tests and batch mode can use them.
lila_module(lila_layouts Scene Layouts.cpp Layouts.h ThemeModel.cpp ThemeModel.h)
target_link_libraries(lila_layouts PUBLIC lila_ops)

lila_module(lila_ui UI
    Actions.cpp
    AdjustLastOp.cpp AdjustLastOp.h
    AppMain.cpp AppMain.h
    FlagFields.cpp FlagFields.h
    OptionBox.cpp OptionBox.h
    MenuSearch.cpp MenuSearch.h
    CommandPalette.cpp CommandPalette.h CommandHistory.h
    TexturePicker.cpp TexturePicker.h
    AgentCapture.cpp AgentCapture.h
    AgentSetup.cpp AgentSetup.h
    Animation.cpp Animation.h
    ApiReference.cpp ApiReference.h
    Docs.cpp Docs.h
    DocsUi.cpp DocsUi.h
    AttributeEditor.cpp AttributeEditor.h
    FileDialog.cpp FileDialog.h
    FontPicker.cpp FontPicker.h MultilineEdit.cpp MultilineEdit.h
    AutoBevelUi.cpp AutoBevelUi.h
    SplitRingsUi.cpp SplitRingsUi.h
    SmartJoinUi.cpp SmartJoinUi.h
    Camera.cpp
    ChannelBox.cpp ChannelBox.h
    CurveTools.cpp CurveToolkit.cpp CurveToolkit.h CurveToolsCommon.h EzCurveTool.cpp CurvePointTool.cpp CurveUi.cpp CurveUi.h PipeTool.cpp CableTool.cpp SectionTool.cpp PlanarCurveUi.cpp
    BlendCurveTool.cpp CurvePick.cpp CurvePick.h TrimCurveTool.cpp VehicleCurveUi.cpp
    SketchTool.cpp SketchUi.cpp SketchUi.h CurveSculptTool.cpp
    DisplayLayersPanel.cpp DisplayLayersPanel.h
    AnimLayersPanel.cpp AnimLayersPanel.h
    OffsetsPanel.cpp OffsetsPanel.h
    RenamePanel.cpp RenamePanel.h
    BatchRunPanel.cpp BatchRunPanel.h
    BakerPanel.cpp BakerPanelMcp.cpp BakerPanel.h
    BakeCageView.cpp BakeCageView.h
    BakePaintTool.cpp BakePaintTool.h
    TexturePaintTool.cpp TexturePaintTool.h
    TexPipelineUi.cpp TexPipelineUi.h
    Editors.cpp Editors.h
    Environments.cpp Environments.h
    ExtensionManagerDialog.cpp ExtensionManagerDialog.h
    GameExporter.cpp GameExporter.h
    MeshSequenceExporter.cpp MeshSequenceExporter.h
    AnimatedSend.cpp AnimatedSend.h
    Ghosting.cpp Ghosting.h
    GnViewerDraw.cpp GnViewerDraw.h
    GraphEditor.cpp GraphEditor.h
    PoseLibrary.cpp PoseLibrary.h
    ControlPicker.cpp ControlPicker.h
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
    MainWindow.cpp MainWindow.h MainWindowDetail.h MainWindowFiles.cpp MainWindowActions.cpp MainWindowMenus.cpp MainWindowLayout.cpp
    Menus.cpp Menus.h
    Modes.cpp Modes.h
    ExtensionUi.cpp ExtensionUi.h
    PythonSdk.cpp
    PythonViewport.cpp
    Owner.h
    SpinScrub.cpp SpinScrub.h
    ValueReset.h
    NodeLookup.h
    Manipulator.cpp Manipulator.h
    Gizmo.cpp Gizmo.h ViewInfo.cpp ViewInfo.h ViewportState.h
    DrawOverrides.cpp DrawOverrides.h RenderPasses.cpp RenderPasses.h SelectionHandlers.cpp SelectionHandlers.h
    ExtrudeHandles.cpp ExtrudeHandles.h
    MaterialDraw.cpp MaterialDraw.h
    MaterialEditor.cpp MaterialEditor.h
    MeasureTool.cpp MeasureTool.h
    DebugDrawUi.cpp DebugDrawUi.h
    PlacePrimitive.cpp PlacePrimitive.h
    MeshCache.cpp MeshCache.h
    SurfaceSnap.cpp SurfaceSnap.h
    GeometrySnap.cpp GeometrySnap.h
    Isolate.cpp Isolate.h
    ModelingToolkit.cpp ModelingToolkit.h
    SoftSelectPanel.cpp SoftSelectPanel.h
    ModifierStack.cpp ModifierStack.h
    NodeEditor.cpp NodeEditor.h NodeCanvas.cpp NodeCanvas.h NodeCanvasLayout.cpp GraphBackend.cpp GraphBackend.h GnGraphBackend.cpp
    TexGraphBackend.cpp TexParams.cpp TexParams.h TexPreview.cpp TexPreview.h Texture2DView.cpp TextureGraphUi.h TextureSetPanel.cpp TextureSetPanel.h
    Overlay.cpp Overlay.h
    CursorBadges.cpp CursorBadges.h
    Panels.cpp Panels.h
    RetopoDrawTool.cpp RetopoDrawTool.h
    RetopoPolesTool.cpp RetopoPolesTool.h
    TopologyTool.cpp TopologyTool.h TopologyModes.cpp TopologyToolkit.cpp TopologyToolkit.h
    KnifeTool.cpp KnifeTool.h
    TargetWeldTool.cpp TargetWeldTool.h
    ArrayTool.cpp ArrayTool.h
    ConformTool.cpp ConformTool.h
    PythonBridge.cpp PythonBridge.h
    RigViewport.cpp RigViewport.h WeightPaintTool.cpp WeightPaintTool.h
    SpacesUi.cpp SpacesUi.h
    InstancingUi.cpp InstancingUi.h
    UnrealMappingUi.cpp UnrealMappingUi.h
    DeformUi.cpp DeformUi.h
    DeformHandles.cpp DeformHandles.h
    Deletion.cpp Deletion.h
    MorphPanel.cpp MorphPanel.h
    SculptPanel.cpp SculptPanel.h
    SculptTool.cpp SculptTool.h
    Tools.cpp Tools.h
    Outliner.cpp Outliner.h
    OutlinerFilter.cpp OutlinerFilter.h
    TokenCompleter.cpp TokenCompleter.h
    WheelGuard.cpp WheelGuard.h
    SetsUi.cpp SetsUi.h
    NotesPanel.cpp NotesPanel.h
    Presenter.cpp Presenter.h
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
    TexLive.cpp TexLive.h
    MaterialBrowser.cpp MaterialBrowser.h MaterialThumbnails.cpp MaterialThumbnails.h
    TextureCache.cpp TextureCache.h
    Theme.cpp Theme.h ThemeEditor.cpp ThemeEditor.h
    TimeSlider.cpp TimeSlider.h
    UiTools.cpp
    UndoHistory.cpp UndoHistory.h
    UvEditor.cpp UvEditor.h
    UvTexturePaint.cpp UvTexturePaint.h
    UvTools.cpp UvTools.h
    UvLive.cpp UvLive.h
    UvPackPanel.cpp UvPackPanel.h
    UvProjectTool.cpp UvProjectTool.h
    FlowLayout.cpp FlowLayout.h
    MenuButtons.cpp MenuButtons.h
    DockSidebar.cpp DockSidebar.h
    LocateFlash.cpp LocateFlash.h
    DockDragCancel.cpp DockDragCancel.h
    DominantInfluenceUi.cpp DominantInfluenceUi.h
    AnimProxyUi.cpp AnimProxyUi.h
    ViewportHooks.cpp ViewportHooks.h
    ViewportDisplay.cpp ViewportDisplay.h
    ViewportHud.cpp ViewportHud.h
    LodDisplay.cpp LodDisplay.h
    ViewportContext.cpp ViewportContext.h
    ViewportLayout.cpp ViewportLayout.h
    ViewportToolbar.cpp ViewportToolbar.h
    DisplayAids.cpp DisplayAids.h
    IconBars.cpp IconBars.h Arrange.h
    ToolBox.cpp ToolBox.h
    VertexPaintTool.cpp VertexPaintTool.h
    PaintSelect.cpp PaintSelect.h
    ViewportTool.cpp ViewportTool.h StepHistory.h SimPreview.cpp SimPreview.h
    ViewportWidget.cpp ViewportWidget.h ViewportWidgetDetail.h ViewportComponents.cpp ViewportViews.cpp ViewportInput.cpp)

set_target_properties(lila_ui PROPERTIES AUTOMOC ON)
target_link_libraries(lila_ui PUBLIC lila_scene lila_io lila_python lila_mcp lila_render lila_editor lila_vcs Qt6::Widgets)
target_link_libraries(lila_ui PUBLIC lila_term)
target_link_libraries(lila_ui PUBLIC lila_layouts)
target_link_libraries(lila_ui PUBLIC lila_uv_cmds)
target_link_libraries(lila_ui PUBLIC lila_gn)
target_link_libraries(lila_ui PUBLIC lila_tex)
target_link_libraries(lila_ui PUBLIC lila_retopo_cmds)
target_link_libraries(lila_ui PUBLIC lila_sculpt_cmds)
target_link_libraries(lila_ui PUBLIC lila_vcolor)
target_link_libraries(lila_ui PRIVATE Python::Python Qt6::Svg)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_ui PRIVATE Qt6::Network miniz::miniz)
