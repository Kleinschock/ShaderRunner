#include "PostProcessChain.h"

#include <osg/Geode>
#include <osg/Vec3>
#include <osg/Vec2>

#include <iostream>
#include <fstream>
#include <sstream>

// ── Helper ──────────────────────────────────────────────────────────────────
static std::string readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        std::cerr << "[PostProcessChain] ERROR: Cannot open file: "
                  << path << "\n";
        return {};
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// ============================================================================
PostProcessChain::PostProcessChain(unsigned int width, unsigned int height,
                                   const std::string& shaderDir)
    : m_width(width), m_height(height), m_shaderDir(shaderDir)
{
    loadCommonSources();
}

// ============================================================================
void PostProcessChain::loadCommonSources()
{
    m_vertexSource = readFile(m_shaderDir + "/fullscreen_quad.vert");
    m_utilsSource  = readFile(m_shaderDir + "/noise_utils.glsl");

    if (m_vertexSource.empty())
        std::cerr << "[PostProcessChain] WARNING: Vertex shader empty.\n";
    if (m_utilsSource.empty())
        std::cerr << "[PostProcessChain] WARNING: noise_utils.glsl empty.\n";
}

// ============================================================================
void PostProcessChain::addEffect(std::shared_ptr<INoiseEffect> effect)
{
    m_effects.push_back(std::move(effect));
}

// ============================================================================
osg::ref_ptr<osg::Group> PostProcessChain::build(osg::ref_ptr<osg::Node> scene)
{
    osg::ref_ptr<osg::Group> root = new osg::Group;

    // ── Scene RTT camera (renders 3D scene to texture) ──────────────────
    osg::ref_ptr<osg::Texture2D> sceneTexture = new osg::Texture2D;
    sceneTexture->setTextureSize(m_width, m_height);
    sceneTexture->setInternalFormat(GL_RGBA);
    sceneTexture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
    sceneTexture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
    sceneTexture->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
    sceneTexture->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);

    osg::ref_ptr<osg::Camera> sceneCamera = new osg::Camera;
    sceneCamera->setClearColor(osg::Vec4(0.1f, 0.1f, 0.15f, 1.0f));
    sceneCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    sceneCamera->setRenderOrder(osg::Camera::PRE_RENDER, 0);
    sceneCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    sceneCamera->setViewport(0, 0, m_width, m_height);
    sceneCamera->attach(osg::Camera::COLOR_BUFFER0, sceneTexture);
    sceneCamera->setReferenceFrame(osg::Transform::RELATIVE_RF);
    sceneCamera->addChild(scene);

    root->addChild(sceneCamera);

    // ── Filter out disabled effects ─────────────────────────────────────
    std::vector<std::shared_ptr<INoiseEffect>> activeEffects;
    for (auto& e : m_effects)
    {
        if (e->isEnabled())
            activeEffects.push_back(e);
    }

    if (activeEffects.empty())
    {
        std::cerr << "[PostProcessChain] WARNING: No enabled effects.\n";
        return root;
    }

    // ── Build effect passes ─────────────────────────────────────────────
    osg::ref_ptr<osg::Texture2D> currentInput = sceneTexture;

    for (size_t i = 0; i < activeEffects.size(); ++i)
    {
        bool isFinal = (i == activeEffects.size() - 1);
        Pass pass = createPass(currentInput, activeEffects[i], isFinal);

        // Set render order: intermediate passes are PRE_RENDER with
        // increasing order index; the final pass is POST_RENDER.
        if (!isFinal)
        {
            pass.camera->setRenderOrder(osg::Camera::PRE_RENDER,
                                        static_cast<int>(i) + 1);
        }

        root->addChild(pass.camera);

        // Register update callbacks
        osg::ref_ptr<osg::NodeCallback> cb = activeEffects[i]->createUpdateCallback();
        if (cb)
            root->addUpdateCallback(cb);

        std::cout << "[PostProcessChain] Pass " << i << ": "
                  << activeEffects[i]->getName()
                  << (isFinal ? " (final)" : "") << "\n";

        currentInput = pass.outputTexture; // may be nullptr for final pass
    }

    return root;
}

// ============================================================================
PostProcessChain::Pass PostProcessChain::createPass(
    osg::ref_ptr<osg::Texture2D> inputTexture,
    std::shared_ptr<INoiseEffect> effect,
    bool isFinalPass)
{
    Pass pass;
    pass.effect = effect;

    // ── Output texture (not needed for final pass) ──────────────────────
    if (!isFinalPass)
    {
        pass.outputTexture = new osg::Texture2D;
        pass.outputTexture->setTextureSize(m_width, m_height);
        pass.outputTexture->setInternalFormat(GL_RGBA);
        pass.outputTexture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
        pass.outputTexture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
        pass.outputTexture->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
        pass.outputTexture->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);
    }

    // ── Camera ──────────────────────────────────────────────────────────
    pass.camera = new osg::Camera;
    pass.camera->setClearMask(0);
    pass.camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    pass.camera->setProjectionMatrix(osg::Matrix::ortho2D(0.0, 1.0, 0.0, 1.0));
    pass.camera->setViewMatrix(osg::Matrix::identity());

    if (isFinalPass)
    {
        pass.camera->setRenderOrder(osg::Camera::POST_RENDER);
    }
    else
    {
        pass.camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        pass.camera->setViewport(0, 0, m_width, m_height);
        pass.camera->attach(osg::Camera::COLOR_BUFFER0, pass.outputTexture);
    }

    // Disable depth and lighting
    osg::StateSet* camSS = pass.camera->getOrCreateStateSet();
    camSS->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
    camSS->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

    // ── Fullscreen quad ─────────────────────────────────────────────────
    pass.quadGeom = createFullscreenQuad();
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(pass.quadGeom);
    pass.camera->addChild(geode);

    // ── Shader program ──────────────────────────────────────────────────
    // Build the fragment source: #version + noise_utils + effect source
    std::string fragBody = effect->getFragmentSource();

    // Strip #version from the effect source (we'll add our own)
    // The effect source starts with #version 330 core
    std::string fragSource;
    auto versionPos = fragBody.find("#version");
    if (versionPos != std::string::npos)
    {
        auto lineEnd = fragBody.find('\n', versionPos);
        std::string versionLine = fragBody.substr(versionPos, lineEnd - versionPos + 1);
        std::string body = fragBody.substr(lineEnd + 1);
        fragSource = versionLine + "\n" + m_utilsSource + "\n" + body;
    }
    else
    {
        fragSource = "#version 330 core\n" + m_utilsSource + "\n" + fragBody;
    }

    osg::ref_ptr<osg::Shader> vertShader =
        new osg::Shader(osg::Shader::VERTEX, m_vertexSource);
    osg::ref_ptr<osg::Shader> fragShader =
        new osg::Shader(osg::Shader::FRAGMENT, fragSource);

    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->setName(effect->getName());
    program->addShader(vertShader);
    program->addShader(fragShader);
    program->addBindAttribLocation("osg_Vertex", 0);
    program->addBindAttribLocation("osg_MultiTexCoord0", 1);

    // ── State setup ─────────────────────────────────────────────────────
    osg::StateSet* ss = pass.quadGeom->getOrCreateStateSet();
    ss->setAttributeAndModes(program, osg::StateAttribute::ON);
    ss->setTextureAttributeAndModes(0, inputTexture, osg::StateAttribute::ON);
    ss->addUniform(new osg::Uniform("u_inputTexture", 0));

    // Let the effect set up its own uniforms
    effect->setupUniforms(ss);

    return pass;
}

// ============================================================================
osg::ref_ptr<osg::Geometry> PostProcessChain::createFullscreenQuad()
{
    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;

    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array(4);
    (*verts)[0].set(0.0f, 0.0f, 0.0f);
    (*verts)[1].set(1.0f, 0.0f, 0.0f);
    (*verts)[2].set(1.0f, 1.0f, 0.0f);
    (*verts)[3].set(0.0f, 1.0f, 0.0f);
    geom->setVertexArray(verts);

    osg::ref_ptr<osg::Vec2Array> texCoords = new osg::Vec2Array(4);
    (*texCoords)[0].set(0.0f, 0.0f);
    (*texCoords)[1].set(1.0f, 0.0f);
    (*texCoords)[2].set(1.0f, 1.0f);
    (*texCoords)[3].set(0.0f, 1.0f);
    geom->setTexCoordArray(0, texCoords);

    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array(1);
    (*normals)[0].set(0.0f, 0.0f, 1.0f);
    geom->setNormalArray(normals, osg::Array::BIND_OVERALL);

    geom->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
    return geom;
}
