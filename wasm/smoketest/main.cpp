// Minimal Ogre-on-wasm smoke test: brings up the real Ogre GLES2 render system
// on the EmscriptenEGLWindow and presents to the page <canvas>, cycling the
// viewport clear colour every frame. Proves the engine -> WebGL2 pipeline is
// live and dynamic in the browser, independent of the rest of the RoR stack.
#include <Ogre.h>
#include <OgreGLES2Plugin.h>
#include <emscripten.h>
#include <cmath>

using namespace Ogre;

namespace {
    Root*        gRoot  = nullptr;
    RenderWindow* gWin  = nullptr;
    Viewport*    gVp    = nullptr;
    GLES2Plugin* gGLES2 = nullptr;
    float        gT     = 0.0f;

    void frame() {
        gT += 0.016f;
        const float r = 0.5f + 0.5f * std::sin(gT);
        const float g = 0.5f + 0.5f * std::sin(gT + 2.094f);
        const float b = 0.5f + 0.5f * std::sin(gT + 4.188f);
        gVp->setBackgroundColour(ColourValue(r, g, b, 1.0f));
        gRoot->renderOneFrame();
    }
}

int main() {
    gRoot = new Root("", "", "ror_smoke.log");

    gGLES2 = new GLES2Plugin();
    gRoot->installPlugin(gGLES2);

    const RenderSystemList& renderers = gRoot->getAvailableRenderers();
    if (renderers.empty()) {
        LogManager::getSingleton().logMessage("SMOKE: no render systems!");
        return 1;
    }
    gRoot->setRenderSystem(renderers.front());
    gRoot->initialise(false);

    NameValuePairList params;
    gWin = gRoot->createRenderWindow("RoR wasm smoke", 800, 600, false, &params);
    gWin->setActive(true);

    SceneManager* scene = gRoot->createSceneManager(ST_GENERIC, "scene");
    Camera* cam = scene->createCamera("cam");
    gVp = gWin->addViewport(cam);
    gVp->setBackgroundColour(ColourValue(0, 0, 0, 1));

    LogManager::getSingleton().logMessage("SMOKE: render window up, entering main loop");
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
