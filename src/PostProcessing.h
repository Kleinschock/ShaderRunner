#pragma once
// ============================================================================
//  PostProcessing — Generic post-processing framework for OpenSceneGraph
// ============================================================================
//  Sets up a render-to-texture pre-render camera and a fullscreen-quad HUD
//  camera so that a fragment shader can process the rendered scene.
// ============================================================================

#include <osg/Group>
#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/StateSet>
#include <osg/ref_ptr>

class PostProcessing
{
public:
    /// Construct the post-processing pipeline.
    /// @param width   Viewport / FBO width  in pixels
    /// @param height  Viewport / FBO height in pixels
    PostProcessing(unsigned int width, unsigned int height);
    virtual ~PostProcessing() = default;

    /// Returns the root group containing both cameras.
    /// Add the original scene as a child of getRTTCamera().
    osg::ref_ptr<osg::Group> getRoot() const { return m_root; }

    /// The pre-render camera that draws the scene into the FBO.
    osg::ref_ptr<osg::Camera> getRTTCamera() const { return m_rttCamera; }

    /// The HUD camera that displays the fullscreen quad.
    osg::ref_ptr<osg::Camera> getHUDCamera() const { return m_hudCamera; }

    /// The FBO color texture (scene output).
    osg::ref_ptr<osg::Texture2D> getSceneTexture() const { return m_sceneTexture; }

    /// The fullscreen quad's geometry (attach shaders to its StateSet).
    osg::ref_ptr<osg::Geometry> getQuadGeometry() const { return m_quadGeom; }

    /// Attach the scene subgraph to the RTT camera.
    void setScene(osg::ref_ptr<osg::Node> scene);

    /// Assign a shader program to the fullscreen quad.
    void setShaderProgram(osg::ref_ptr<osg::Program> program);

    unsigned int getWidth()  const { return m_width;  }
    unsigned int getHeight() const { return m_height; }

protected:
    void createRTTCamera();
    void createHUDCamera();
    osg::ref_ptr<osg::Geometry> createFullscreenQuad();

    unsigned int m_width;
    unsigned int m_height;

    osg::ref_ptr<osg::Group>     m_root;
    osg::ref_ptr<osg::Camera>    m_rttCamera;
    osg::ref_ptr<osg::Camera>    m_hudCamera;
    osg::ref_ptr<osg::Texture2D> m_sceneTexture;
    osg::ref_ptr<osg::Geometry>  m_quadGeom;
};
