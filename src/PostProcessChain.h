#pragma once
// ============================================================================
//  PostProcessChain — Multi-pass post-processing pipeline for OSG
// ============================================================================
//  Chains multiple INoiseEffect passes together. Each effect gets its own
//  RTT camera and fullscreen quad.  The output texture of pass N becomes
//  the input texture of pass N+1.  The final pass renders to screen.
// ============================================================================

#include "PostProcessing.h"
#include "INoiseEffect.h"

#include <osg/Group>
#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>

#include <vector>
#include <memory>
#include <string>

class PostProcessChain
{
public:
    PostProcessChain(unsigned int width, unsigned int height,
                     const std::string& shaderDir = "shaders");

    /// Add an effect to the end of the chain.
    void addEffect(std::shared_ptr<INoiseEffect> effect);

    /// Build the complete scene graph.
    /// Call this AFTER adding all effects.
    /// @param scene  The 3D scene to render
    /// @return Root group to set as the viewer's scene data
    osg::ref_ptr<osg::Group> build(osg::ref_ptr<osg::Node> scene);

    unsigned int getWidth()  const { return m_width;  }
    unsigned int getHeight() const { return m_height; }

private:
    /// Load noise_utils.glsl and the fullscreen quad vertex shader source.
    void loadCommonSources();

    /// Build a single pass (RTT camera + fullscreen quad + shader).
    struct Pass
    {
        osg::ref_ptr<osg::Camera>    camera;
        osg::ref_ptr<osg::Texture2D> outputTexture;
        osg::ref_ptr<osg::Geometry>  quadGeom;
        std::shared_ptr<INoiseEffect> effect;
    };

    Pass createPass(osg::ref_ptr<osg::Texture2D> inputTexture,
                    std::shared_ptr<INoiseEffect> effect,
                    bool isFinalPass);

    osg::ref_ptr<osg::Geometry> createFullscreenQuad();

    unsigned int m_width;
    unsigned int m_height;
    std::string  m_shaderDir;

    std::string  m_vertexSource;
    std::string  m_utilsSource;

    std::vector<std::shared_ptr<INoiseEffect>> m_effects;
};
