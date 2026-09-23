#include "Flyback.h"

/**
    The source: the machine on its own background, no input.

    **This file is listed directly in the FlybackSource target, not in
    flyback_core.** Both plugins share the class; what they do not share is the
    `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one: `CFFGLPluginInfo` registers itself from a file-scope constructor that
    nothing references by name, so in an archive the linker may drop the whole
    translation unit -- a bundle that loads, exports `plugMain`, and reports
    that it contains no plugins.

    The name is `SW Flyback`, ten characters. The FFGL name field is 16 bytes
    and NOT null-terminated, so a longer one is truncated without a word;
    `oxbow probe` reads it back the way a host does.
*/
namespace
{
class FlybackSource : public flyback::FlybackPlugin
{
public:
	FlybackSource() :
		FlybackPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlybackSource >,                   // Create method
	"HV01",                                           // Plugin unique ID of maximum length 4
	"SW Flyback",                                     // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_SOURCE,                                        // Plugin type
	"High-voltage discharges: a Jacob's ladder, a Tesla coil, a Van de Graaff, a plasma globe and a Lichtenberg "
	"figure, every one grown by the dielectric breakdown model on a real Laplace solve, with each machine's own "
	"circuit deciding when the air breaks and how much light it gives.\n\nStart from a Preset, at the top.",
	"Flyback FFGL source"                             // About
);

extern "C" const char* FlybackSourceBuildStamp()
{
	return "flyback " FLYBACK_VERSION " source, built " __DATE__ " " __TIME__;
}
