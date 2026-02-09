#pragma once
// ============================================================================
//  SensorNoiseSimulator — Convenience assembler for all noise effects
// ============================================================================
//  Chains the four noise modules in physically correct order:
//    1. PRNU        (multiplicative, applied to clean signal first)
//    2. Dark noise  (additive dark current + DSNU + hot pixels)
//    3. Photon noise (Poisson shot noise on total signal)
//    4. Read noise   (additive Gaussian from readout)
//
//  Each module can be independently enabled/disabled and adjusted.
// ============================================================================

#include "PostProcessChain.h"
#include "PRNUEffect.h"
#include "DarkNoiseEffect.h"
#include "PhotonNoiseEffect.h"
#include "ReadNoiseEffect.h"

#include <osgGA/GUIEventHandler>
#include <memory>
#include <iostream>

class SensorNoiseSimulator
{
public:
    SensorNoiseSimulator(unsigned int width, unsigned int height,
                         const std::string& shaderDir = "shaders")
        : m_chain(width, height, shaderDir)
    {
        // Create effect modules
        m_prnu       = std::make_shared<PRNUEffect>(shaderDir);
        m_darkNoise  = std::make_shared<DarkNoiseEffect>(shaderDir);
        m_photonNoise = std::make_shared<PhotonNoiseEffect>(shaderDir);
        m_readNoise  = std::make_shared<ReadNoiseEffect>(shaderDir);

        // Set resolution on all effects
        float w = static_cast<float>(width);
        float h = static_cast<float>(height);
        m_prnu->setResolution(w, h);
        m_darkNoise->setResolution(w, h);
        m_photonNoise->setResolution(w, h);
        m_readNoise->setResolution(w, h);

        // Add in physically correct order
        m_chain.addEffect(m_prnu);
        m_chain.addEffect(m_darkNoise);
        m_chain.addEffect(m_photonNoise);
        m_chain.addEffect(m_readNoise);
    }

    /// Build the scene graph with all effects applied.
    osg::ref_ptr<osg::Group> apply(osg::ref_ptr<osg::Node> scene)
    {
        return m_chain.build(scene);
    }

    // ── Direct access to each module ────────────────────────────────────
    std::shared_ptr<PRNUEffect>&        prnu()        { return m_prnu; }
    std::shared_ptr<DarkNoiseEffect>&   darkNoise()   { return m_darkNoise; }
    std::shared_ptr<PhotonNoiseEffect>& photonNoise() { return m_photonNoise; }
    std::shared_ptr<ReadNoiseEffect>&   readNoise()   { return m_readNoise; }

    /// Get an event handler for interactive control.
    osg::ref_ptr<osgGA::GUIEventHandler> getEventHandler();

private:
    PostProcessChain m_chain;

    std::shared_ptr<PRNUEffect>        m_prnu;
    std::shared_ptr<DarkNoiseEffect>   m_darkNoise;
    std::shared_ptr<PhotonNoiseEffect> m_photonNoise;
    std::shared_ptr<ReadNoiseEffect>   m_readNoise;
};

// ── Keyboard handler ────────────────────────────────────────────────────
class SensorNoiseKeyHandler : public osgGA::GUIEventHandler
{
public:
    SensorNoiseKeyHandler(SensorNoiseSimulator& sim) : m_sim(sim) {}

    bool handle(const osgGA::GUIEventAdapter& ea,
                osgGA::GUIActionAdapter&) override
    {
        if (ea.getEventType() != osgGA::GUIEventAdapter::KEYDOWN)
            return false;

        switch (ea.getKey())
        {
        // ── Photon scale ────────────────────────────────────────────
        case '+':
        case osgGA::GUIEventAdapter::KEY_KP_Add:
        {
            float s = m_sim.photonNoise()->getPhotonScale() * 1.5f;
            m_sim.photonNoise()->setPhotonScale(s);
            std::cout << "[Sensor] Photon scale: " << s << "  (less shot noise)\n";
            return true;
        }
        case '-':
        case osgGA::GUIEventAdapter::KEY_KP_Subtract:
        {
            float s = m_sim.photonNoise()->getPhotonScale() / 1.5f;
            m_sim.photonNoise()->setPhotonScale(s);
            std::cout << "[Sensor] Photon scale: " << s << "  (more shot noise)\n";
            return true;
        }

        // ── Dark current ────────────────────────────────────────────
        case 'd':
        {
            float v = std::max(0.001f, m_sim.darkNoise()->getDarkCurrent() * 2.0f);
            m_sim.darkNoise()->setDarkCurrent(v);
            std::cout << "[Sensor] Dark current: " << v << "\n";
            return true;
        }
        case 'D':
        {
            float v = m_sim.darkNoise()->getDarkCurrent() * 0.5f;
            m_sim.darkNoise()->setDarkCurrent(v);
            std::cout << "[Sensor] Dark current: " << v << "\n";
            return true;
        }

        // ── Read noise ──────────────────────────────────────────────
        case 'n':
        {
            float v = std::max(0.001f, m_sim.readNoise()->getReadNoise() * 1.5f);
            m_sim.readNoise()->setReadNoise(v);
            std::cout << "[Sensor] Read noise: " << v << "\n";
            return true;
        }
        case 'N':
        {
            float v = m_sim.readNoise()->getReadNoise() / 1.5f;
            m_sim.readNoise()->setReadNoise(v);
            std::cout << "[Sensor] Read noise: " << v << "\n";
            return true;
        }

        // ── PRNU ────────────────────────────────────────────────────
        case 'p':
        {
            float v = std::max(0.001f, m_sim.prnu()->getPRNUStrength() * 1.5f);
            m_sim.prnu()->setPRNUStrength(v);
            std::cout << "[Sensor] PRNU: " << (v * 100.f) << " %\n";
            return true;
        }
        case 'P':
        {
            float v = m_sim.prnu()->getPRNUStrength() / 1.5f;
            m_sim.prnu()->setPRNUStrength(v);
            std::cout << "[Sensor] PRNU: " << (v * 100.f) << " %\n";
            return true;
        }

        // ── DSNU ────────────────────────────────────────────────────
        case 's':
        {
            float v = std::max(0.001f, m_sim.darkNoise()->getDSNUStrength() * 1.5f);
            m_sim.darkNoise()->setDSNUStrength(v);
            std::cout << "[Sensor] DSNU: " << v << "\n";
            return true;
        }
        case 'S':
        {
            float v = m_sim.darkNoise()->getDSNUStrength() / 1.5f;
            m_sim.darkNoise()->setDSNUStrength(v);
            std::cout << "[Sensor] DSNU: " << v << "\n";
            return true;
        }

        // ── Reset all ───────────────────────────────────────────────
        case 'r':
        case 'R':
        {
            m_sim.photonNoise()->setPhotonScale(100.0f);
            m_sim.darkNoise()->setDarkCurrent(0.005f);
            m_sim.darkNoise()->setDSNUStrength(0.003f);
            m_sim.darkNoise()->setHotPixelProbability(0.0005f);
            m_sim.darkNoise()->setHotPixelStrength(50.0f);
            m_sim.readNoise()->setReadNoise(0.01f);
            m_sim.prnu()->setPRNUStrength(0.01f);
            std::cout << "[Sensor] All parameters reset to defaults\n";
            return true;
        }

        // ── Toggle individual effects ───────────────────────────────
        case '1':
            m_sim.prnu()->setEnabled(!m_sim.prnu()->isEnabled());
            std::cout << "[Sensor] PRNU " << (m_sim.prnu()->isEnabled() ? "ON" : "OFF") << "\n";
            return true;
        case '2':
            m_sim.darkNoise()->setEnabled(!m_sim.darkNoise()->isEnabled());
            std::cout << "[Sensor] Dark noise " << (m_sim.darkNoise()->isEnabled() ? "ON" : "OFF") << "\n";
            return true;
        case '3':
            m_sim.photonNoise()->setEnabled(!m_sim.photonNoise()->isEnabled());
            std::cout << "[Sensor] Photon noise " << (m_sim.photonNoise()->isEnabled() ? "ON" : "OFF") << "\n";
            return true;
        case '4':
            m_sim.readNoise()->setEnabled(!m_sim.readNoise()->isEnabled());
            std::cout << "[Sensor] Read noise " << (m_sim.readNoise()->isEnabled() ? "ON" : "OFF") << "\n";
            return true;

        default:
            return false;
        }
    }

private:
    SensorNoiseSimulator& m_sim;
};

inline osg::ref_ptr<osgGA::GUIEventHandler> SensorNoiseSimulator::getEventHandler()
{
    return new SensorNoiseKeyHandler(*this);
}
