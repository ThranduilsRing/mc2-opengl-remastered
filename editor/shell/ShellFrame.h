#pragma once
#include "FileWorkflow.h"

namespace editor::shell {
// Accepted close ends editing immediately, even when accepted by the document UI.
template<class DocumentUI, class PanelUI>
bool drawWhileOpen(const FileWorkflow& files, DocumentUI documentUI, PanelUI panelUI)
{
    if (!files.closing()) documentUI();
    if (!files.closing()) panelUI();
    return !files.closing();
}
}
