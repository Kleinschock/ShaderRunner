#pragma once
// ============================================================================
//  INoiseEffect — Interface for modular noise effects
// ============================================================================
//  Each noise type implements this interface.  The PostProcessChain uses
//  it to build a multi-pass pipeline.
// ============================================================================

#include <osg/StateSet>
#include <osg/NodeCallback>
#include <osg/ref_ptr>
#include <string>

class INoiseEffect
{
public:
    virtual ~INoiseEffect() = default;

    /// Return the fragment shader source (without #version or noise_utils).
    /// The chain will prepend #version and noise_utils.glsl automatically.
    virtual std::string getFragmentSource() const = 0;

    /// Attach effect-specific uniforms to the given StateSet.
    virtual void setupUniforms(osg::StateSet* ss) = 0;

    /// Optional per-frame update callback (e.g. frame counter).
    /// Return nullptr if not needed.
    virtual osg::ref_ptr<osg::NodeCallback> createUpdateCallback() { return nullptr; }

    /// Human-readable name for logging.
    virtual std::string getName() const = 0;

    /// Is this effect currently enabled?
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }

protected:
    bool m_enabled = true;
};
