#pragma once
#include <QStringView>

// Card chrome follows the content's capabilities. Keep this list in sync with
// specs/presentation.md; menus and overlays must agree about available actions.
struct PreviewPresentation {
    bool fit=false;
    bool resetCamera=false;
    bool integratedHeader=false;
};
inline PreviewPresentation previewPresentation(QStringView kind) {
    if(kind==u"model" || kind==u"material") return {true,true,false};
    if(kind==u"image" || kind==u"graph") return {true,false,false};
    if(kind==u"file" || kind==u"selection") return {false,false,true};
    return {};
}
