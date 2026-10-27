#include "../src/window_placement.h"
#include <cassert>

int main() {
    const QList<QRect> screens={{0,30,1920,1050},{-1280,0,1280,1024}};
    assert(fitWindowFrame({100,100,1000,720},screens)==QRect(100,100,1000,720));
    assert(fitWindowFrame({-1200,100,1000,720},screens)==QRect(-1200,100,1000,720));
    assert(fitWindowFrame({1800,900,1000,720},screens)==QRect(920,360,1000,720));
    assert(fitWindowFrame({4000,2000,1000,720},screens)==QRect(920,360,1000,720));
    assert(fitWindowFrame({0,0,3000,2000},screens)==QRect(0,30,1920,1050));
    assert(fitWindowFrame({-1100,-100,1000,720},screens)==QRect(-1100,0,1000,720));
    assert(fitWindowFrame({10,10,800,600},{})==QRect(10,10,800,600));
}
