#pragma once
// Shared local-only libmpv setup. Audio owns no video renderer or GL context.
#include <QByteArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <mpv/client.h>
#include <stdexcept>

namespace preview::media {
inline void checked(int code) { if (code<0) throw std::runtime_error(mpv_error_string(code)); }
inline void command(mpv_handle* player, const QByteArray& first, const QByteArray& second={}, const QByteArray& third={}) {
    const char* args[]={first.constData(),second.isNull()?nullptr:second.constData(),third.isNull()?nullptr:third.constData(),nullptr};
    checked(mpv_command_async(player,0,args));
}
inline void flag(mpv_handle* player, const char* name, bool value) {
    int v=value; checked(mpv_set_property_async(player,0,name,MPV_FORMAT_FLAG,&v));
}
inline void string(mpv_handle* player, const char* name, const char* value) {
    checked(mpv_set_property_async(player,0,name,MPV_FORMAT_STRING,&value));
}
inline void configure(mpv_handle* player, bool audio) {
    const auto option=[&](const char* key,const char* value) {
        const int code=mpv_set_option_string(player,key,value);
        // Minimal LGPL builds can omit scripting entirely.
        if (code==MPV_ERROR_OPTION_NOT_FOUND &&
            (qstrcmp(key,"load-scripts")==0 || qstrcmp(key,"ytdl")==0 || qstrcmp(key,"osc")==0)) return;
        checked(code);
    };
    option("config","no"); option("load-scripts","no"); option("ytdl","no");
    option("input-default-bindings","no"); option("input-vo-keyboard","no"); option("osc","no"); option("osd-level","0");
    option("vo",audio?"null":"libmpv"); option("hwdec",audio?"no":"auto-safe"); option("keep-open","yes");
    option("background","color"); option("idle","yes"); option("cache","no");
    option("demuxer-max-bytes","8MiB"); option("demuxer-max-back-bytes","0");
    option("audio-display","no"); option("sub-auto","no"); option("audio-file-auto","no");
    option("access-references","no"); option("msg-level","all=error");
    // A candidate cannot emit sound before it replaces the last-good player.
    // Audio keeps its decoder selected even when muted, to establish readiness.
    option("mute","yes"); option("aid",audio?"auto":"no");
    if (audio) option("vid","no");
    // Tests explicitly choose a silent output, never the desktop's audio sink.
    if (qEnvironmentVariableIntValue("ASSET_PREVIEW_TEST_NULL_AUDIO")==1) option("ao","null");
}
inline void updateProperty(QJsonObject& properties, const mpv_event_property& prop) {
    if (!prop.data) { properties.remove(prop.name); return; }
    switch (prop.format) {
    case MPV_FORMAT_FLAG: properties[prop.name]=bool(*static_cast<int*>(prop.data)); break;
    case MPV_FORMAT_INT64: properties[prop.name]=double(*static_cast<int64_t*>(prop.data)); break;
    case MPV_FORMAT_DOUBLE: properties[prop.name]=*static_cast<double*>(prop.data); break;
    case MPV_FORMAT_STRING: properties[prop.name]=QString::fromUtf8(*static_cast<char**>(prop.data)); break;
    default: break;
    }
}
inline bool playbackKey(const QKeyEvent* e) {
    if (e->modifiers()==Qt::ShiftModifier) return e->key()==Qt::Key_Left || e->key()==Qt::Key_Right;
    return e->modifiers()==Qt::NoModifier &&
        (e->key()==Qt::Key_Space || e->key()==Qt::Key_M || e->key()==Qt::Key_Home);
}
}
