#include "PostProcessing.h"

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Texture2D>
#include <osg/Vec3>
#include <osg/Vec2>

// ============================================================================
PostProcessing::PostProcessing(unsigned int width, unsigned int height)
    : m_width(width), m_height(height)
{
    m_root = new osg::Group;

    // Create the FBO color texture
    m_sceneTexture = new osg::Texture2D;
    m_sceneTexture->setTextureSize(m_width, m_height);
    m_sceneTexture->setInternalFormat(GL_RGBA);
    m_sceneTexture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);
    m_sceneTexture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::LINEAR);
    m_sceneTexture->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::CLAMP_TO_EDGE);
    m_sceneTexture->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::CLAMP_TO_EDGE);

    createRTTCamera();
    createHUDCamera();

    m_root->addChild(m_rttCamera);
    m_root->addChild(m_hudCamera);
}

// ============================================================================
void PostProcessing::createRTTCamera()
{
    m_rttCamera = new osg::Camera;

    m_rttCamera->setClearColor(osg::Vec4(0.1f, 0.1f, 0.15f, 1.0f));
    m_rttCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_rttCamera->setRenderOrder(osg::Camera::PRE_RENDER);
    m_rttCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    m_rttCamera->setViewport(0, 0, m_width, m_height);

    // Attach the color texture to the FBO
    m_rttCamera->attach(osg::Camera::COLOR_BUFFER0, m_sceneTexture);
}

// ============================================================================
void PostProcessing::createHUDCamera()
{
    m_hudCamera = new osg::Camera;

    m_hudCamera->setClearMask(0);           // No clearing, we fill every pixel
    m_hudCamera->setRenderOrder(osg::Camera::POST_RENDER);
    m_hudCamera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    m_hudCamera->setProjectionMatrix(
        osg::Matrix::ortho2D(0.0, 1.0, 0.0, 1.0));
    m_hudCamera->setViewMatrix(osg::Matrix::identity());

    // Disable depth testing on the HUD
    m_hudCamera->getOrCreateStateSet()->setMode(
        GL_DEPTH_TEST, osg::StateAttribute::OFF);
    m_hudCamera->getOrCreateStateSet()->setMode(
        GL_LIGHTING, osg::StateAttribute::OFF);

    // Create and attach fullscreen quad
    m_quadGeom = createFullscreenQuad();

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(m_quadGeom);
    m_hudCamera->addChild(geode);

    // Bind the scene texture to texture unit 0
    osg::StateSet* ss = m_quadGeom->getOrCreateStateSet();
    ss->setTextureAttributeAndModes(0, m_sceneTexture,
        osg::StateAttribute::ON);
}

// ============================================================================
osg::ref_ptr<osg::Geometry> PostProcessing::createFullscreenQuad()
{
    osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;

    // Quad corners: [0,1] × [0,1]
    osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array(4);
    (*verts)[0].set(0.0f, 0.0f, 0.0f);
    (*verts)[1].set(1.0f, 0.0f, 0.0f);
    (*verts)[2].set(1.0f, 1.0f, 0.0f);
    (*verts)[3].set(0.0f, 1.0f, 0.0f);
    geom->setVertexArray(verts);

    // Texture coordinates matching the quad
    osg::ref_ptr<osg::Vec2Array> texCoords = new osg::Vec2Array(4);
    (*texCoords)[0].set(0.0f, 0.0f);
    (*texCoords)[1].set(1.0f, 0.0f);
    (*texCoords)[2].set(1.0f, 1.0f);
    (*texCoords)[3].set(0.0f, 1.0f);
    geom->setTexCoordArray(0, texCoords);

    // Normals (not really needed, but keeps OSG happy)
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array(1);
    (*normals)[0].set(0.0f, 0.0f, 1.0f);
    geom->setNormalArray(normals, osg::Array::BIND_OVERALL);

    // Draw as a quad strip
    geom->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));

    return geom;
}

// ============================================================================
void PostProcessing::setScene(osg::ref_ptr<osg::Node> scene)
{
    // Remove existing children from RTT camera (keep it clean)
    m_rttCamera->removeChildren(0, m_rttCamera->getNumChildren());
    m_rttCamera->addChild(scene);
}

// ============================================================================
void PostProcessing::setShaderProgram(osg::ref_ptr<osg::Program> program)
{
    osg::StateSet* ss = m_quadGeom->getOrCreateStateSet();
    ss->setAttributeAndModes(program, osg::StateAttribute::ON);
}
