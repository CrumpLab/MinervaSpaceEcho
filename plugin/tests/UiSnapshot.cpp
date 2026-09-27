// Renders the editor to a PNG without a window: plays generated audio through
// the plugin so memory has something in it, then draws the editor.
//   mse-ui-snapshot out.png [width height] [preset name] [row to select]
#include "../src/PluginEditor.h"
#include "../src/PluginProcessor.h"
#include "TestSignals.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: mse-ui-snapshot out.png [width height] [preset] [row] [probe|probe-next]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const int width = argc > 3 ? std::atoi (argv[2]) : 1040;
    const int height = argc > 3 ? std::atoi (argv[3]) : 760;

    MinervaSpaceEchoProcessor processor;
    if (argc > 4)
    {
        for (const auto& p : processor.getPresets())
            if (p.name == juce::String (argv[4]))
                processor.applyPreset (p);
    }
    processor.setPlayConfigDetails (2, 2, 48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    // 12 bars of the style-change clip (drums then a new style).
    mse::testgen::Options o;
    o.bars = 12;
    const auto clip = mse::testgen::styleChange (o);
    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;
    const int total = clip.numSamples() - 12000; // stop partway into the last bar
    for (int pos = 0; pos + 512 <= total; pos += 512)
    {
        for (int c = 0; c < 2; ++c)
            block.copyFrom (c, 0, clip.channels[static_cast<size_t> (c)].data() + pos, 512);
        processor.processBlock (block, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (width, height);
    auto* ed = dynamic_cast<MinervaSpaceEchoEditor*> (editor.get());
    ed->refresh();
    if (argc > 5)
        ed->selectRow (std::atoi (argv[5]));
    if (argc > 6 && juce::String (argv[6]).startsWith ("probe"))
    {
        // Probe with the selected row (Stage 11), optionally comparing with n-1.
        if (const auto* v = processor.readMemoryView(); v != nullptr && argc > 5 && std::atoi (argv[5]) < v->count)
        {
            processor.sendCommand (mse::Command::ProbeTrace, v->rows[static_cast<size_t> (std::atoi (argv[5]))].serial);
            if (juce::String (argv[6]) == "probe-next")
                processor.sendCommand (mse::Command::ProbeCompare, 1);
        }
        block.clear();
        for (int k = 0; k < 20; ++k)
            processor.processBlock (block, midi);
    }
    ed->refresh();
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::printf ("could not write %s\n", argv[1]);
        return 1;
    }
    std::printf ("wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), width, height);
    return 0;
}
