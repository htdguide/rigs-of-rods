/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2020 Petr Ohlidal

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "TerrainGeometryManager.h"

#include "Actor.h"
#include "Application.h"
#include "ContentManager.h"
#include "Language.h"
#include "GfxScene.h"
#include "GUIManager.h"
#include "GUI_LoadingWindow.h"
#include "Terrain.h"
#include "Terrn2FileFormat.h"
#include "ShadowManager.h"
#include "OgreTerrainPSSMMaterialGenerator.h"
#include "OTCFileFormat.h"

#include <OgreLight.h>
#include <Terrain/OgreTerrainGroup.h>

using namespace Ogre;
using namespace RoR;

#define CUSTOM_MAT_PROFILE_NAME "Terrn2CustomMat"

/// @author: http://www.ogre3d.org/forums/viewtopic.php?f=5&t=72455
class Terrn2CustomMaterial : public Ogre::TerrainMaterialGenerator
{
public:

    Terrn2CustomMaterial(Ogre::String materialName, bool addNormalmap, bool cloneMaterial) 
      : m_material_name(materialName), m_add_normal_map(addNormalmap), m_clone_material(cloneMaterial)
    {
        mProfiles.push_back(OGRE_NEW Profile(this, CUSTOM_MAT_PROFILE_NAME, "Renders RoR terrn2 with custom material"));
        this->setActiveProfile(CUSTOM_MAT_PROFILE_NAME);
    }

    void setMaterialByName(const Ogre::String materialName)
    {
        m_material_name = materialName;
        this->_markChanged();
    };

    class Profile : public Ogre::TerrainMaterialGenerator::Profile
    {
    public:
        Profile(Ogre::TerrainMaterialGenerator* parent, const Ogre::String& name, const Ogre::String& desc)
        : Ogre::TerrainMaterialGenerator::Profile(parent, name, desc)
        {
        };
        ~Profile() override {};

        bool               isVertexCompressionSupported () const { return false; }
        void               setLightmapEnabled           (bool set) /*override*/ {} // OGRE 1.8 doesn't have this method
        Ogre::MaterialPtr  generate                     (const Ogre::Terrain* terrain) override;
        Ogre::uint8        getMaxLayers                 (const Ogre::Terrain* terrain) const override { return 0; };
        void               updateParams                 (const Ogre::MaterialPtr& mat, const Ogre::Terrain* terrain) override {};
        void               updateParamsForCompositeMap  (const Ogre::MaterialPtr& mat, const Ogre::Terrain* terrain) override {};

        Ogre::MaterialPtr generateForCompositeMap(const Ogre::Terrain* terrain) override
        {
            return terrain->_getCompositeMapMaterial();
        };

        void requestOptions(Ogre::Terrain* terrain) override
        {
            terrain->_setMorphRequired(false);
            terrain->_setNormalMapRequired(true); // enable global normal map
            terrain->_setLightMapRequired(false);
            terrain->_setCompositeMapRequired(false);
        };
    };

protected:
    Ogre::String m_material_name;
    bool m_clone_material;
    bool m_add_normal_map;
};

Ogre::MaterialPtr Terrn2CustomMaterial::Profile::generate(const Ogre::Terrain* terrain)
{
    const Ogre::String& matName = terrain->getMaterialName();

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(matName);
    if (mat) 
        Ogre::MaterialManager::getSingleton().remove(matName);

    Terrn2CustomMaterial* parent = static_cast<Terrn2CustomMaterial*>(this->getParent());

    // Set Ogre material 
    mat = Ogre::MaterialManager::getSingleton().getByName(parent->m_material_name);

    // Clone material
    if(parent->m_clone_material)
    {
        mat = mat->clone(matName);
        parent->m_material_name = matName;
    }

    // Add normalmap
    if(parent->m_add_normal_map)
    {
        // Get default pass
        Ogre::Pass *p = mat->getTechnique(0)->getPass(0);

        // Add terrain's global normalmap to renderpass so the fragment program can find it.
        Ogre::TextureUnitState *tu = p->createTextureUnitState(matName+"/nm");

        Ogre::TexturePtr nmtx = terrain->getTerrainNormalMap();
        tu->_setTexturePtr(nmtx);
    }

    return mat;
};

// ----------------------------------------------------------------------------

#ifdef __EMSCRIPTEN__
// Ogre's built-in terrain material generator (TerrainPSSMMaterialGenerator)
// emits SM2 shaders whose GLSL ES variant fails to even preprocess on WebGL2,
// leaving the terrain blank/white. Generate a minimal, lit, single-texture
// material instead and let the RTSS (already installed in GfxScene) produce the
// GLSL ES shader from this fixed-function-style pass.
struct WasmTerrainLayer
{
    std::string diffuse;     // diffuse_specular texture filename
    std::string blendmap;    // blendmap image filename (empty for layer 0)
    float       world_size = 1.f;
    float       alpha = 1.f;
    char        blend_mode = 'R'; // which channel of the blendmap carries this layer's coverage
};

class WasmTerrainMaterialGenerator : public Ogre::TerrainMaterialGenerator
{
public:
    // Layer data comes from RoR's parsed .otc. We DON'T declare Ogre terrain layer
    // samplers: that makes the Terrain load the layer textures on a background
    // WorkQueue thread, but WebGL texture uploads must run on the main thread
    // (emscripten proxies them) and the main thread is blocked inside the
    // synchronous loadAllTerrains() -> deadlock. Instead we reference the diffuse
    // and blendmap textures by name in the material below; they then load lazily
    // via the normal material/resource path on the main thread.
    //
    // Lighting uses Ogre's global terrain normal map (world-space per-pixel normal
    // derived from the heightfield), so this works for both flat and hilly terrain.
    static const size_t MAX_LAYERS = 6;

    explicit WasmTerrainMaterialGenerator(std::vector<WasmTerrainLayer> layers)
        : m_layers(std::move(layers))
    {
        if (m_layers.size() > MAX_LAYERS)
            m_layers.resize(MAX_LAYERS);
        mProfiles.push_back(OGRE_NEW Profile(this, "WasmSimple", "Blended lit GLES2 terrain"));
        this->setActiveProfile("WasmSimple");
    }

    std::vector<WasmTerrainLayer> m_layers;

    class Profile : public Ogre::TerrainMaterialGenerator::Profile
    {
    public:
        Profile(Ogre::TerrainMaterialGenerator* parent, const Ogre::String& name, const Ogre::String& desc)
            : Ogre::TerrainMaterialGenerator::Profile(parent, name, desc) {}
        ~Profile() override {}

        bool        isVertexCompressionSupported() const { return false; }
        void        setLightmapEnabled(bool) {}
        Ogre::uint8 getMaxLayers(const Ogre::Terrain*) const override { return 1; }
        void        updateParams(const Ogre::MaterialPtr&, const Ogre::Terrain*) override {}
        void        updateParamsForCompositeMap(const Ogre::MaterialPtr&, const Ogre::Terrain*) override {}

        Ogre::MaterialPtr generateForCompositeMap(const Ogre::Terrain* terrain) override
        {
            return this->generate(terrain);
        }

        void requestOptions(Ogre::Terrain* terrain) override
        {
            terrain->_setMorphRequired(false);
            // Global normal map: world-space per-pixel normals for slope shading.
            terrain->_setNormalMapRequired(true);
            terrain->_setLightMapRequired(false);
            terrain->_setCompositeMapRequired(false);
        }

        // Build (once) the GLSL ES vertex/fragment programs for an N-layer blended,
        // globally-normal-mapped terrain. The fragment program is generated for the
        // exact layer count so the unrolled blend has no dynamic sampler indexing
        // (illegal in GLSL ES 100). Program names are keyed by layer count.
        static bool ensurePrograms(size_t numLayers)
        {
            auto& hmgr = Ogre::HighLevelGpuProgramManager::getSingleton();
            const std::string vpName = "RoRWasm/TerrainVP";
            const std::string fpName = "RoRWasm/TerrainFP" + std::to_string(numLayers);
            if (hmgr.getByName(vpName, Ogre::RGN_DEFAULT) && hmgr.getByName(fpName, Ogre::RGN_DEFAULT))
                return true;

            const char* VS_SRC =
                "#version 100\n"
                "attribute vec4 vertex;\n"
                "attribute vec2 uv0;\n"
                "uniform mat4 worldViewProj;\n"
                "varying vec2 vUV;\n"          // 0..1 across the whole terrain page
                "void main() {\n"
                "  gl_Position = worldViewProj * vertex;\n"
                "  vUV = uv0;\n"
                "}\n";

            // Fragment program: base layer + (N-1) blended layers, then lit by the
            // global normal map. Blendmaps are sampled untiled (0..1); diffuse maps
            // are tiled by per-layer 'tilingN'.
            std::string fs;
            fs += "#version 100\n";
            fs += "precision mediump float;\n";
            for (size_t i = 0; i < numLayers; ++i)
                fs += "uniform sampler2D diff" + std::to_string(i) + ";\n";
            for (size_t i = 1; i < numLayers; ++i)
                fs += "uniform sampler2D blend" + std::to_string(i) + ";\n";
            for (size_t i = 0; i < numLayers; ++i)
                fs += "uniform float tiling" + std::to_string(i) + ";\n";
            for (size_t i = 1; i < numLayers; ++i)
                fs += "uniform vec4 chan" + std::to_string(i) + ";\n"; // channel mask * alpha
            fs += "uniform sampler2D normalMap;\n";
            fs += "uniform vec4 lightDir;\n";       // world-space direction the light travels
            fs += "uniform vec4 lightDiffuse;\n";
            fs += "uniform vec4 ambient;\n";
            fs += "varying vec2 vUV;\n";
            fs += "void main() {\n";
            fs += "  vec3 col = texture2D(diff0, vUV * tiling0).rgb;\n";
            for (size_t i = 1; i < numLayers; ++i)
            {
                std::string si = std::to_string(i);
                fs += "  float w" + si + " = clamp(dot(texture2D(blend" + si + ", vUV), chan" + si + "), 0.0, 1.0);\n";
                fs += "  col = mix(col, texture2D(diff" + si + ", vUV * tiling" + si + ").rgb, w" + si + ");\n";
            }
            fs += "  vec3 nWS = normalize(texture2D(normalMap, vUV).xyz * 2.0 - 1.0);\n";
            fs += "  vec3 L = normalize(-lightDir.xyz);\n";
            fs += "  float ndl = max(dot(nWS, L), 0.0);\n";
            fs += "  gl_FragColor = vec4(col * (ambient.rgb + lightDiffuse.rgb * ndl), 1.0);\n";
            fs += "}\n";

            try
            {
                if (!hmgr.getByName(vpName, Ogre::RGN_DEFAULT))
                {
                    auto vp = hmgr.createProgram(vpName, Ogre::RGN_DEFAULT, "glsles", Ogre::GPT_VERTEX_PROGRAM);
                    vp->setSource(VS_SRC);
                }
                auto fp = hmgr.createProgram(fpName, Ogre::RGN_DEFAULT, "glsles", Ogre::GPT_FRAGMENT_PROGRAM);
                fp->setSource(fs);
                return true;
            }
            catch (Ogre::Exception& e)
            {
                RoR::LogFormat("[RoR|wasm] terrain programs failed: %s", e.getFullDescription().c_str());
                return false;
            }
        }

        Ogre::MaterialPtr generate(const Ogre::Terrain* terrain) override
        {
            const Ogre::String& matName = terrain->getMaterialName();
            Ogre::MaterialManager& mm = Ogre::MaterialManager::getSingleton();
            if (mm.getByName(matName))
                mm.remove(matName);

            Ogre::MaterialPtr mat = mm.create(matName, Ogre::RGN_DEFAULT);
            Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

            WasmTerrainMaterialGenerator* parent =
                static_cast<WasmTerrainMaterialGenerator*>(this->getParent());

            const std::vector<WasmTerrainLayer>& layers = parent->m_layers;
            Ogre::TexturePtr normalMap = terrain->getTerrainNormalMap();
            const size_t n = layers.size();

            // Preferred path: blended, globally-normal-mapped GLSL ES shader. Needs at
            // least a base layer and the global normal map.
            if (n >= 1 && !layers[0].diffuse.empty() && normalMap && ensurePrograms(n))
            {
                pass->setVertexProgram("RoRWasm/TerrainVP");
                pass->setFragmentProgram("RoRWasm/TerrainFP" + std::to_string(n));

                Ogre::GpuProgramParametersSharedPtr fsp = pass->getFragmentProgramParameters();
                int unit = 0;

                // diffuse maps (units 0..n-1)
                for (size_t i = 0; i < n; ++i)
                {
                    Ogre::TextureUnitState* td = pass->createTextureUnitState(layers[i].diffuse);
                    td->setTextureAddressingMode(Ogre::TextureUnitState::TAM_WRAP);
                    fsp->setNamedConstant("diff" + std::to_string(i), unit++);
                }
                // blendmaps (units n..2n-2), for layers 1..n-1
                for (size_t i = 1; i < n; ++i)
                {
                    Ogre::TextureUnitState* tb = layers[i].blendmap.empty()
                        ? pass->createTextureUnitState() // transparent default -> layer hidden
                        : pass->createTextureUnitState(layers[i].blendmap);
                    tb->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
                    fsp->setNamedConstant("blend" + std::to_string(i), unit++);
                }
                // global normal map (last unit)
                {
                    Ogre::TextureUnitState* tn = pass->createTextureUnitState(normalMap->getName());
                    tn->_setTexturePtr(normalMap);
                    tn->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
                    fsp->setNamedConstant("normalMap", unit++);
                }

                // per-layer tiling + channel masks
                const float worldSize = (float)terrain->getWorldSize();
                for (size_t i = 0; i < n; ++i)
                {
                    const float tiling = (layers[i].world_size > 0.f) ? (worldSize / layers[i].world_size) : 1.f;
                    fsp->setNamedConstant("tiling" + std::to_string(i), tiling);
                }
                for (size_t i = 1; i < n; ++i)
                {
                    Ogre::Vector4 mask(0, 0, 0, 0);
                    switch (layers[i].blend_mode)
                    {
                        case 'R': mask.x = 1; break;
                        case 'G': mask.y = 1; break;
                        case 'B': mask.z = 1; break;
                        case 'A': mask.w = 1; break;
                        default:  mask.x = 1; break;
                    }
                    mask *= layers[i].alpha;
                    fsp->setNamedConstant("chan" + std::to_string(i), mask);
                }

                Ogre::GpuProgramParametersSharedPtr vsp = pass->getVertexProgramParameters();
                vsp->setNamedAutoConstant("worldViewProj", Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);

                fsp->setNamedAutoConstant("lightDir",     Ogre::GpuProgramParameters::ACT_LIGHT_DIRECTION, 0);
                fsp->setNamedAutoConstant("lightDiffuse", Ogre::GpuProgramParameters::ACT_LIGHT_DIFFUSE_COLOUR, 0);
                fsp->setNamedAutoConstant("ambient",      Ogre::GpuProgramParameters::ACT_AMBIENT_LIGHT_COLOUR);
                return mat;
            }

            // Fallback: RTSS-generated fixed-function lighting (base diffuse only, or a
            // flat ground colour if no texture).
            pass->setLightingEnabled(true);
            pass->setAmbient(1.f, 1.f, 1.f);
            pass->setDiffuse(1.f, 1.f, 1.f, 1.f);
            pass->setSpecular(0.f, 0.f, 0.f, 0.f);

            if (n >= 1 && !layers[0].diffuse.empty())
            {
                const float tiling = (layers[0].world_size > 0.f) ? (terrain->getWorldSize() / layers[0].world_size) : 1.f;
                Ogre::TextureUnitState* tu = pass->createTextureUnitState(layers[0].diffuse);
                tu->setTextureAddressingMode(Ogre::TextureUnitState::TAM_WRAP);
                if (tiling > 0.f)
                    tu->setTextureScale(1.f / tiling, 1.f / tiling);
            }
            else
            {
                pass->setAmbient(0.55f, 0.55f, 0.5f);
                pass->setDiffuse(0.55f, 0.55f, 0.5f, 1.f);
            }

            return mat;
        }
    };
};
#endif // __EMSCRIPTEN__

#define XZSTR(X,Z)   String("[") + TOSTRING(X) + String(",") + TOSTRING(Z) + String("]")

TerrainGeometryManager::TerrainGeometryManager(Terrain* terrainManager)
    : mHeightData(nullptr)
    , mIsFlat(false)
    , mMinHeight(0.0f)
    , mMaxHeight(std::numeric_limits<float>::min())
    , m_was_new_geometry_generated(false)
    , terrainManager(terrainManager)
    , m_ogre_terrain_group(nullptr)
{
}

TerrainGeometryManager::~TerrainGeometryManager()
{
    if (m_ogre_terrain_group != nullptr)
    {
        m_ogre_terrain_group->removeAllTerrains();
    }
}

/// @author Ported from OGRE engine, www.ogre3d.org, file OgreTerrain.cpp
float TerrainGeometryManager::getHeightAtTerrainPosition(Real x, Real y)
{
    // get left / bottom points (rounded down)
    Real factor = (Real)mSize - 1.0f;
    Real invFactor = 1.0f / factor;

    long startX = static_cast<long>(x * factor);
    long startY = static_cast<long>(y * factor);
    long endX = startX + 1;
    long endY = startY + 1;

    // now get points in terrain space (effectively rounding them to boundaries)
    // note that we do not clamp! We need a valid plane
    Real startXTS = startX * invFactor;
    Real startYTS = startY * invFactor;
    Real endXTS = endX * invFactor;
    Real endYTS = endY * invFactor;

    // get parametric from start coord to next point
    Real xParam = (x * factor - startX);
    Real yParam = (y * factor - startY);

    /* For even / odd tri strip rows, triangles are this shape:
    even     odd
    3---2   3---2
    | / |   | \ |
    0---1   0---1
    */

    // Build all 4 positions in terrain space, using point-sampled height
    Vector3 v0(startXTS, startYTS, mHeightData[startY * mSize + startX]);
    Vector3 v1(endXTS  , startYTS, mHeightData[startY * mSize + endX]);
    Vector3 v2(endXTS  , endYTS  , mHeightData[endY   * mSize + endX]);
    Vector3 v3(startXTS, endYTS  , mHeightData[endY   * mSize + startX]);

    // define this plane in terrain space
    Vector3 normal;
    Real d;
    if (startY % 2)
    {
        // odd row
        bool secondTri = ((1.0 - yParam) > xParam);
        if (secondTri)
        {
            normal = (v1 - v0).crossProduct(v3 - v0);
            d = -normal.dotProduct(v0);
        }
        else
        {
            normal = (v2 - v1).crossProduct(v3 - v1);
            d = -normal.dotProduct(v1);
        }
    }
    else
    {
        // even row
        bool secondTri = (yParam > xParam);
        if (secondTri)
        {
            normal = (v2 - v0).crossProduct(v3 - v0);
            d = -normal.dotProduct(v0);
        }
        else
        {
            normal = (v1 - v0).crossProduct(v2 - v0);
            d = -normal.dotProduct(v0);
        }
    }

    // Solve plane equation for z
    return (-normal.x * x - normal.y * y - d) / normal.z;
}

float TerrainGeometryManager::getHeightAt(float x, float z)
{
    if (m_spec->is_flat)
        return 0.0f;

    float tx = (x - mBase - mPos.x) / ((mSize - 1) *  mScale);
    float ty = (z + mBase - mPos.z) / ((mSize - 1) * -mScale);

    if (tx <= 0.0f || ty <= 0.0f || tx >= 1.0f || ty >= 1.0f)
        return terrainManager->GetDef()->water_bottom_height;
    else if (mIsFlat)
        return mMinHeight;

    return getHeightAtTerrainPosition(tx, ty);
}

Ogre::Vector3 TerrainGeometryManager::getNormalAt(float x, float y, float z)
{
    const float precision = 0.1f;
    Vector3 normal(getHeightAt(x - precision, z) - y, precision, y - getHeightAt(x, z + precision));
    normal.normalise();
    return normal;
}

bool TerrainGeometryManager::InitTerrain(std::string otc_filename)
{
    OTCParser otc_parser;

    // Load main *.otc file
    try
    {
        DataStreamPtr ds_config = ResourceGroupManager::getSingleton().openResource(otc_filename);
        if (!ds_config || !ds_config->isReadable())
        {
            RoR::LogFormat("[RoR|Terrain] Cannot read main *.otc file [%s].", otc_filename.c_str());
            return false;
        }
        if (!otc_parser.LoadMasterConfig(ds_config, otc_filename.c_str()))
        {
            return false; // Error already reported
        }
    }
    catch (...)
    {
        RoR::HandleGenericException(fmt::format("TerrainGeometryManager::InitTerrain({})", otc_filename));
        // If we stop parsing we might break some legacy maps
        //return false;
    }

    // Load *.otc files for pages
    for (OTCPage& page : otc_parser.GetDefinition()->pages)
    {
        if (page.pageconf_filename.empty())
        {
            continue; // For backwards compatibility.
        }

        try
        {
            DataStreamPtr ds_page = ResourceGroupManager::getSingleton().openResource(page.pageconf_filename);
            if (!ds_page || !ds_page->isReadable())
            {
                RoR::LogFormat("[RoR|Terrain] Cannot read file [%s].", page.pageconf_filename.c_str());
                return false;
            }

            // NOTE: Empty file is accepted (leaving all values to defaults) for backwards compatibility.
            if (!otc_parser.LoadPageConfig(ds_page, page, page.pageconf_filename.c_str()))
            {
                return false; // Error already logged
            }
        }
        catch (...)
        {
            RoR::HandleGenericException(fmt::format("TerrainGeometryManager::InitTerrain({})", page.pageconf_filename));
            // If we stop parsing we might break some legacy maps
            // return false;
        }
    }

    m_spec = otc_parser.GetDefinition();

    const std::string cache_filename_format = m_spec->cache_filename_base + "_OGRE_" + TOSTRING(OGRE_VERSION) + "_";

    m_ogre_terrain_group = OGRE_NEW TerrainGroup(App::GetGfxScene()->GetSceneManager(), Ogre::Terrain::ALIGN_X_Z, m_spec->page_size, m_spec->world_size);
    m_ogre_terrain_group->setFilenameConvention(cache_filename_format, "mapbin");
    m_ogre_terrain_group->setOrigin(m_spec->origin_pos);
    m_ogre_terrain_group->setResourceGroup(RGN_CACHE);

    configureTerrainDefaults();

    for (OTCPage& page : m_spec->pages)
    {
        this->SetupGeometry(page, m_spec->is_flat);
    }

    // sync load since we want everything in place when we start
    App::GetGuiManager()->LoadingWindow.SetProgress(44, _L("Loading terrain pages ..."));
    m_ogre_terrain_group->loadAllTerrains(true);

    Ogre::Terrain* terrain = m_ogre_terrain_group->getTerrain(0, 0);

    if (terrain == nullptr)
        return true;

    mHeightData = terrain->getHeightData();
    mSize = terrain->getSize();
    const float world_size = terrain->getWorldSize();
    mBase = -world_size * 0.5f;
    mScale = world_size / (Real)(mSize - 1);
    mPos = terrain->getPosition();

    // terrain->getMinHeight() / terrain->getMaxHeight() seem to be unreliable ~ ulteq 12/18
    for (int x = 0; x < mSize; x++)
    {
        for (int y = 0; y < mSize; y++)
        {
            float h = mHeightData[y * mSize + x];
            mMinHeight = std::min(h, mMinHeight);
            mMaxHeight = std::max(mMaxHeight, h);
        }
    }
    mIsFlat = std::abs(mMaxHeight - mMinHeight) < std::numeric_limits<float>::epsilon();

    if (m_was_new_geometry_generated)
    {
        // update the blend maps
        if (terrainManager->GetDef()->custom_material_name.empty())
        {
            for (OTCPage& page : m_spec->pages)
            {
                Ogre::Terrain* terrain = m_ogre_terrain_group->getTerrain(page.pos_x, page.pos_z);

                if (terrain != nullptr)
                {
                    this->SetupLayers(page, terrain);
                    this->SetupBlendMaps(page, terrain);
                }
            }
        }

        // always save the results when it was imported
        if (!m_spec->disable_cache)
        {
            App::GetGuiManager()->LoadingWindow.SetProgress(50, _L("Saving all terrain pages ..."));
            m_ogre_terrain_group->saveAllTerrains(false);
        }
    }
    else
    {
        LOG(" *** Terrain loaded from cache ***");
    }

    m_ogre_terrain_group->freeTemporaryResources();
    return true;
}

void TerrainGeometryManager::updateLightMap()
{
    TerrainGroup::TerrainIterator ti = m_ogre_terrain_group->getTerrainIterator();

    while (ti.hasMoreElements())
    {
        Ogre::Terrain* terrain = ti.getNext()->instance;
        if (!terrain)
            continue;

        if (!terrain->isDerivedDataUpdateInProgress())
        {
            terrain->dirtyLightmap();
            terrain->updateDerivedData();
        }
    }
}

void TerrainGeometryManager::UpdateMainLightPosition()
{
    Light* light = terrainManager->getMainLight();
    TerrainGlobalOptions* terrainOptions = TerrainGlobalOptions::getSingletonPtr();
    if (light)
    {
        terrainOptions->setLightMapDirection(light->getDerivedDirection());
        terrainOptions->setCompositeMapDiffuse(light->getDiffuseColour());
    }
    terrainOptions->setCompositeMapAmbient(App::GetGfxScene()->GetSceneManager()->getAmbientLight());

    m_ogre_terrain_group->update();
}

void TerrainGeometryManager::configureTerrainDefaults()
{
    if (!TerrainGlobalOptions::getSingletonPtr())
    {
        OGRE_NEW TerrainGlobalOptions();
    }

    TerrainGlobalOptions* terrainOptions = TerrainGlobalOptions::getSingletonPtr();
    std::string const & custom_mat = terrainManager->GetDef()->custom_material_name;
    if (!custom_mat.empty())
    {
        terrainOptions->setDefaultMaterialGenerator(
            Ogre::TerrainMaterialGeneratorPtr(new Terrn2CustomMaterial(custom_mat, false, true)));
    }
    else
    {
#ifdef __EMSCRIPTEN__
        // The SM2/PSSM generator's shaders don't work on WebGL2 - use a custom
        // GLSL ES material that blends all .otc layers by their blendmaps and lights
        // them with the global terrain normal map. Feed it the parsed .otc layers so
        // it can reference the textures by name (avoiding Ogre's worker-thread layer
        // loading, which deadlocks on WebGL).
        std::vector<WasmTerrainLayer> wasm_layers;
        if (!m_spec->pages.empty())
        {
            for (const RoR::OTCLayer& layer : m_spec->pages.begin()->layers)
            {
                WasmTerrainLayer wl;
                wl.diffuse    = layer.diffusespecular_filename;
                wl.blendmap   = layer.blendmap_filename;
                wl.world_size = layer.world_size;
                wl.alpha      = layer.alpha;
                wl.blend_mode = layer.blend_mode;
                wasm_layers.push_back(wl);
            }
        }
        terrainOptions->setDefaultMaterialGenerator(
            Ogre::TerrainMaterialGeneratorPtr(new WasmTerrainMaterialGenerator(std::move(wasm_layers))));
#else
        terrainOptions->setDefaultMaterialGenerator(
            Ogre::TerrainMaterialGeneratorPtr(new Ogre::TerrainPSSMMaterialGenerator()));
#endif
    }
    // Configure global
    terrainOptions->setMaxPixelError(m_spec->max_pixel_error);

    // Important to set these so that the terrain knows what to use for derived (non-realtime) data
    Light* light = terrainManager->getMainLight();
    if (light)
    {
        terrainOptions->setLightMapDirection(light->getDerivedDirection());
        if (custom_mat.empty())
        {
            terrainOptions->setCompositeMapDiffuse(light->getDiffuseColour());
        }
    }
    terrainOptions->setCompositeMapAmbient(App::GetGfxScene()->GetSceneManager()->getAmbientLight());

    // Configure default import settings for if we use imported image
    Ogre::Terrain::ImportData& defaultimp = m_ogre_terrain_group->getDefaultImportSettings();
    defaultimp.terrainSize  = m_spec->page_size; // the heightmap size
    defaultimp.worldSize    = m_spec->world_size; // this is the scaled up size, like 12km
    defaultimp.inputScale   = m_spec->world_size_y;
    defaultimp.minBatchSize = m_spec->batch_size_min;
    defaultimp.maxBatchSize = m_spec->batch_size_max;

    // optimizations
    // NOTE: the block below assumes the default generator is a
    // TerrainPSSMMaterialGenerator (its active profile is an SM2Profile). On the
    // web build we install WasmTerrainMaterialGenerator instead, so this cast
    // would be invalid (calling SM2Profile methods through the wrong vtable hangs
    // the terrain load). Skip the SM2-specific configuration there.
#ifdef __EMSCRIPTEN__
    const bool use_sm2_profile = false;
#else
    const bool use_sm2_profile = custom_mat.empty();
#endif
    TerrainPSSMMaterialGenerator::SM2Profile* matProfile = nullptr;
    if (use_sm2_profile)
    {
        matProfile = static_cast<TerrainPSSMMaterialGenerator::SM2Profile*>(terrainOptions->getDefaultMaterialGenerator()->getActiveProfile());
        if (matProfile)
        {
            matProfile->setLightmapEnabled(m_spec->lightmap_enabled);
            // Fix for OpenGL, otherwise terrains are black
            if (Root::getSingleton().getRenderSystem()->getName() == "OpenGL Rendering Subsystem")
            {
                matProfile->setLayerNormalMappingEnabled(true);
                matProfile->setLayerSpecularMappingEnabled(true);
            }
            else
            {
                matProfile->setLayerNormalMappingEnabled(m_spec->norm_map_enabled);
                matProfile->setLayerSpecularMappingEnabled(m_spec->spec_map_enabled);
            }
            matProfile->setLayerParallaxMappingEnabled(m_spec->parallax_enabled);
            matProfile->setGlobalColourMapEnabled(m_spec->global_colormap_enabled);
            matProfile->setReceiveDynamicShadowsDepth(m_spec->recv_dyn_shadows_depth);

            terrainManager->getShadowManager()->updateTerrainMaterial(matProfile);
        }
    }

    terrainOptions->setLayerBlendMapSize   (m_spec->layer_blendmap_size);
    terrainOptions->setCompositeMapSize    (m_spec->composite_map_size);
    terrainOptions->setCompositeMapDistance(m_spec->composite_map_distance);
    terrainOptions->setSkirtSize           (m_spec->skirt_size);
    terrainOptions->setLightMapSize        (m_spec->lightmap_size);

    if (use_sm2_profile && matProfile)
    {
        if (matProfile->getReceiveDynamicShadowsPSSM())
        {
            terrainOptions->setCastsDynamicShadows(true);
        }
    }

    terrainOptions->setUseRayBoxDistanceCalculation(false);

    //TODO: Make this only when hydrax is enabled.
    terrainOptions->setUseVertexCompressionWhenAvailable(false);

    // HACK: Load the single page config now
    // This is how it "worked before" ~ only_a_ptr, 04/2017
    if (!m_spec->pages.empty())
    {
        this->SetupLayers(*m_spec->pages.begin(), nullptr);
    }
}

// if terrain is set, we operate on the already loaded terrain
void TerrainGeometryManager::SetupLayers(RoR::OTCPage& page, Ogre::Terrain *terrain)
{
    if (page.num_layers == 0)
        return;

    Ogre::Terrain::ImportData& defaultimp = m_ogre_terrain_group->getDefaultImportSettings();

    if (!terrain)
        defaultimp.layerList.resize(page.num_layers);

    int layer_idx = 0;

    for (OTCLayer& layer : page.layers)
    {
        if (!terrain)
        {
            defaultimp.layerList[layer_idx].worldSize = layer.world_size;
            defaultimp.layerList[layer_idx].textureNames.push_back(layer.diffusespecular_filename);
            defaultimp.layerList[layer_idx].textureNames.push_back(layer.normalheight_filename);
        }
        else
        {
            terrain->setLayerWorldSize(layer_idx, layer.world_size);
            terrain->setLayerTextureName(layer_idx, 0, layer.diffusespecular_filename);
            terrain->setLayerTextureName(layer_idx, 1, layer.normalheight_filename);
        }

        layer_idx++;
    }
    LOG("done loading page: loaded " + TOSTRING(layer_idx) + " layers");
}

void TerrainGeometryManager::SetupBlendMaps(OTCPage& page, Ogre::Terrain* terrain )
{
    const int layerCount = terrain->getLayerCount();
    auto layer_def_itor = page.layers.begin();

    if (page.layers.size() < 2)
    {
        LOG(fmt::format("[RoR|Terrain] Page {}-{} has no blend layers defined, blendmap will not be set up.", page.pos_x, page.pos_z));
        return;
    }

    ++layer_def_itor;
    for (int i = 1; i < layerCount; i++)
    {
        if (layer_def_itor->blendmap_filename.empty())
            continue;

        Ogre::Image img;
        try
        {
            img.load(layer_def_itor->blendmap_filename, ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        }
        catch (Exception& e)
        {
            LOG("Error loading blendmap: " + layer_def_itor->blendmap_filename + " : " + e.getFullDescription());
            continue;
        }

        TerrainLayerBlendMap* blendmap = terrain->getLayerBlendMap(i);

        // resize that blending map so it will fit
        const Ogre::uint32 blendmapSize = terrain->getLayerBlendMapSize();
        if (img.getWidth() != blendmapSize)
            img.resize(blendmapSize, blendmapSize);

        // now to the ugly part
        float* ptr = blendmap->getBlendPointer();
        for (Ogre::uint32 z = 0; z != blendmapSize; z++)
        {
            for (Ogre::uint32 x = 0; x != blendmapSize; x++)
            {
                Ogre::ColourValue c = img.getColourAt(x, z, 0);
                const float alpha = layer_def_itor->alpha;
                if (layer_def_itor->blend_mode == 'R')
                    *ptr++ = c.r * alpha;
                else if (layer_def_itor->blend_mode == 'G')
                    *ptr++ = c.g * alpha;
                else if (layer_def_itor->blend_mode == 'B')
                    *ptr++ = c.b * alpha;
                else if (layer_def_itor->blend_mode == 'A')
                    *ptr++ = c.a * alpha;
            }
        }
        blendmap->dirty();
        blendmap->update();
        ++layer_def_itor;
    }

    if (m_spec->blendmap_dbg_enabled)
    {
        for (int i = 1; i < layerCount; i++)
        {
            Ogre::TerrainLayerBlendMap* blendMap = terrain->getLayerBlendMap(i);
            Ogre::uint32 blendmapSize = terrain->getLayerBlendMapSize();
            Ogre::Image img;
            unsigned short* idata = OGRE_ALLOC_T(unsigned short, blendmapSize * blendmapSize, Ogre::MEMCATEGORY_RESOURCE);
            float scale = 65535.0f;
            for (unsigned int x = 0; x < blendmapSize; x++)
                for (unsigned int z = 0; z < blendmapSize; z++)
                    idata[x + z * blendmapSize] = (unsigned short)(blendMap->getBlendValue(x, blendmapSize - z) * scale);
            img.loadDynamicImage((Ogre::uchar*)(idata), blendmapSize, blendmapSize, Ogre::PF_L16);
            std::string fileName = "blendmap_layer_" + Ogre::StringConverter::toString(i) + ".png";
            img.save(fileName);
            OGRE_FREE(idata, Ogre::MEMCATEGORY_RESOURCE);
        }
    }
}

// Internal helper
bool LoadHeightmap(OTCPage& page, Image& img)
{
    if (page.heightmap_filename.empty())
    {
        LOG("[RoR|Terrain] Empty Heightmap provided in OTC, please use 'Flat=1' instead");
        return false;
    }

    if (page.heightmap_filename.find(".raw") != String::npos)
    {
        // load raw data
        DataStreamPtr stream = ResourceGroupManager::getSingleton().openResource(page.heightmap_filename);
        LOG("[RoR|Terrain] loading RAW image: " + TOSTRING(stream->size()) + " / " + TOSTRING(page.raw_size*page.raw_size*page.raw_bpp));
        PixelFormat pix_format = (page.raw_bpp == 2) ? PF_L16 : PF_L8;
        img.loadRawData(stream, page.raw_size, page.raw_size, 1, pix_format);
    }
    else
    {
        img.load(page.heightmap_filename, ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    if (page.raw_flip_x)
        img.flipAroundX();
    if (page.raw_flip_y)
        img.flipAroundY();

    return true;
}

void TerrainGeometryManager::SetupGeometry(RoR::OTCPage& page, bool flat)
{
    if (flat)
    {
        // very simple, no height data to load at all
        m_ogre_terrain_group->defineTerrain(page.pos_x, page.pos_z, 0.0f);
        return;
    }

    const std::string page_cache_filename = m_ogre_terrain_group->generateFilename(page.pos_x, page.pos_z);
    const std::string res_group = m_ogre_terrain_group->getResourceGroup();
    if (!m_spec->disable_cache && ResourceGroupManager::getSingleton().resourceExists(res_group, page_cache_filename))
    {
        // load from cache
        m_ogre_terrain_group->defineTerrain(page.pos_x, page.pos_z);
    }
    else
    {
        Image img;
        if (LoadHeightmap(page, img))
        {
            m_ogre_terrain_group->defineTerrain(page.pos_x, page.pos_z, &img);
            m_was_new_geometry_generated = true;
        }
        else
        {
            // fall back to no heightmap
            m_ogre_terrain_group->defineTerrain(page.pos_x, page.pos_z, 0.0f);
        }
    }
}

Ogre::Vector3 TerrainGeometryManager::getMaxTerrainSize()
{
    return Vector3(m_spec->world_size_x, mMaxHeight, m_spec->world_size_z);
}

