// ============================================================================
//  Sensor Noise Simulator Demo — Modular Post-Processing with OSG
// ============================================================================
//  Each noise source is a separate, independent module that can be
//  toggled on/off and adjusted at runtime.
//
//  Pipeline (physically correct order):
//    Scene → PRNU → Dark Noise → Photon Noise → Read Noise → Screen
//
//  Controls:
//    +/-       Photon scale       (shot noise level)
//    d/D       Dark current       (increase / decrease)
//    n/N       Read noise         (increase / decrease)
//    p/P       PRNU strength      (increase / decrease)
//    s/S       DSNU strength      (increase / decrease)
//    1-4       Toggle individual effects on/off
//    R         Reset all to defaults
//    Esc       Quit
// ============================================================================

#include "SensorNoiseSimulator.h"

#include <osg/Group>
#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osg/Material>
#include <osg/Light>
#include <osg/LightSource>
#include <osgDB/ReadFile>
#include <osgViewer/Viewer>
#include <osgGA/TrackballManipulator>

#include <iostream>

// ── Build a default lit scene ───────────────────────────────────────────
static osg::ref_ptr<osg::Group> createDefaultScene()
{
    osg::ref_ptr<osg::Group> root = new osg::Group;

    // Sphere
    {
        auto sphere = new osg::Sphere(osg::Vec3(0, 0, 0), 1.0f);
        auto drawable = new osg::ShapeDrawable(sphere);
        drawable->setColor(osg::Vec4(0.8f, 0.3f, 0.2f, 1.0f));

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(drawable);

        auto mat = new osg::Material;
        mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(0.8f, 0.3f, 0.2f, 1.0f));
        mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4(1, 1, 1, 1));
        mat->setShininess(osg::Material::FRONT_AND_BACK, 64.0f);
        geode->getOrCreateStateSet()->setAttributeAndModes(mat);
        root->addChild(geode);
    }

    // Ground plane
    {
        auto box = new osg::Box(osg::Vec3(0, 0, -1.2f), 8.0f, 8.0f, 0.1f);
        auto drawable = new osg::ShapeDrawable(box);
        drawable->setColor(osg::Vec4(0.4f, 0.4f, 0.5f, 1.0f));

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(drawable);
        root->addChild(geode);
    }

    // Second sphere
    {
        auto sphere = new osg::Sphere(osg::Vec3(2.0f, 1.0f, -0.5f), 0.5f);
        auto drawable = new osg::ShapeDrawable(sphere);
        drawable->setColor(osg::Vec4(0.2f, 0.6f, 0.9f, 1.0f));

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(drawable);

        auto mat = new osg::Material;
        mat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(0.2f, 0.6f, 0.9f, 1.0f));
        mat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4(1, 1, 1, 1));
        mat->setShininess(osg::Material::FRONT_AND_BACK, 32.0f);
        geode->getOrCreateStateSet()->setAttributeAndModes(mat);
        root->addChild(geode);
    }

    // Light
    {
        auto light = new osg::Light;
        light->setLightNum(0);
        light->setPosition(osg::Vec4(5, 5, 10, 1));
        light->setDiffuse(osg::Vec4(1.0f, 0.95f, 0.85f, 1.0f));
        light->setAmbient(osg::Vec4(0.15f, 0.15f, 0.2f, 1.0f));
        light->setSpecular(osg::Vec4(1, 1, 1, 1));

        auto ls = new osg::LightSource;
        ls->setLight(light);
        root->addChild(ls);
    }

    return root;
}

// ============================================================================
int main(int argc, char** argv)
{
    const unsigned int WIDTH  = 1280;
    const unsigned int HEIGHT = 720;

    std::cout << "====================================================\n"
              << "  Sensor Noise Simulator — Modular OSG Pipeline\n"
              << "====================================================\n"
              << "  Noise modules (toggle with 1-4):\n"
              << "    1  PRNU         (Photo-Response Non-Uniformity)\n"
              << "    2  Dark Noise   (Dark Current + DSNU + Hot Pixels)\n"
              << "    3  Photon Noise (Poisson Shot Noise)\n"
              << "    4  Read Noise   (Gaussian Readout)\n"
              << "\n"
              << "  Parameter controls:\n"
              << "    +/-   Photon scale    d/D   Dark current\n"
              << "    n/N   Read noise      p/P   PRNU\n"
              << "    s/S   DSNU            R     Reset all\n"
              << "====================================================\n\n";

    // Load or create scene
    osg::ref_ptr<osg::Node> scene;
    if (argc > 1)
    {
        scene = osgDB::readNodeFile(argv[1]);
        if (!scene)
            std::cerr << "[Main] Could not load: " << argv[1] << "\n\n";
    }
    if (!scene)
        scene = createDefaultScene();

    // Create modular sensor noise simulator
    SensorNoiseSimulator simulator(WIDTH, HEIGHT);
    osg::ref_ptr<osg::Group> root = simulator.apply(scene);

    // Set up viewer
    osgViewer::Viewer viewer;
    viewer.setSceneData(root);
    viewer.setUpViewInWindow(100, 100, WIDTH, HEIGHT);
    viewer.setCameraManipulator(new osgGA::TrackballManipulator);
    viewer.addEventHandler(simulator.getEventHandler());

    return viewer.run();
}
