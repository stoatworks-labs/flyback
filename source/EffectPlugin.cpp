#include "Flyback.h"

/**
    The effect: the same machines over the clip, striking into it.

    The clip is the ground. Its luma (or alpha, or edges -- Detect On) is
    thresholded onto the breakdown model's lattice, so bolts reach for the
    bright parts of the picture, arcs bridge between highlights and the plasma
    globe's filaments gather to the brightest thing in frame. The flash lights
    the clip, too (Illumination).

    See SourcePlugin.cpp for why this file is listed in its own target rather
    than in the shared library.

    The name is `SW Flyback Over`, fifteen characters, one short of the 16-byte
    field the host reads without a terminator.
*/
namespace
{
class FlybackEffect : public flyback::FlybackPlugin
{
public:
	FlybackEffect() :
		FlybackPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlybackEffect >,                   // Create method
	"HV02",                                           // Plugin unique ID of maximum length 4
	"SW Flyback Over",                                // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"High-voltage discharges over the clip, with the clip as the ground: bolts strike its bright parts, arcs "
	"bridge its highlights and the flash lights it. Grown by the dielectric breakdown model on a real Laplace "
	"solve.\n\nStart from a Preset, at the top.",
	"Flyback FFGL effect"                             // About
);

extern "C" const char* FlybackEffectBuildStamp()
{
	return "flyback " FLYBACK_VERSION " effect, built " __DATE__ " " __TIME__;
}
