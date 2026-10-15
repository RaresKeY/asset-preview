#include "../src/model_frame.h"
#include <cassert>
int main() {
    using preview::ModelFrame;
    const QJsonObject ship{{"up_axis","y"},{"front_axis","-z"}};
    const ModelFrame f(ship);
    assert(f.front==QVector3D(0,0,-1));
    assert(f.direction("right")==QVector3D(1,0,0));
    assert(f.direction("top")==QVector3D(0,1,0));
    assert(f.direction("back")==QVector3D(0,0,1));
    const auto d=f.direction("isometric"); assert(d.x()>0 && d.y()>0 && d.z()<0);
    assert(ModelFrame({}).direction("front")==QVector3D(0,0,1));
    assert(ModelFrame(QJsonObject{{"up_axis","z"}}).direction("front")==QVector3D(0,-1,0));
    assert(!preview::frameChanged(ship,QJsonObject{{"up_axis","y"},{"front_axis","-z"},{"materials",false}}));
    assert(preview::frameChanged(ship,QJsonObject{{"up_axis","y"},{"front_axis","+z"}}));
    assert(!preview::frameChanged({},QJsonObject{{"up_axis","y"}}));
}
