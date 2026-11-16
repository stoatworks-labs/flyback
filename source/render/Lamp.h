#pragma once

#include "Singing.h"

#include <FFGLSDK.h>

/**
    The singing coil's status lamp: a small disc in a dark bezel in the
    frame's top-left corner, drawn last, over everything, and ONLY while Pitch
    Source is on. With the mode off not one GL call is made here, and the shader
    is not even compiled (it compiles on first use, and a lamp that will not
    compile is logged, never fatal: the plugin draws on without it).

    The colours are spasis's and earshot's, as rubens and eidophone show them:
    green Running; red Silent (open, and bit-exact zeros: no permission, an
    unpatched input); blinking red Missing or Failed; blinking amber while
    nothing is open yet. Blinking is 2 Hz on the host's clock.

    It lives outside Shaders.cpp on purpose: that file is the light path, and
    the browser demo carries a character-for-character copy of it
    (demo/tools/check_shaders.py).
*/
namespace flyback
{
class LampPainter
{
public:
	struct Placement
	{
		bool drawn   = false;
		float x      = 0.0f, y = 0.0f;///< the centre, window pixels (GL: y up)
		float radius = 0.0f;           ///< the lit disc, pixels; the bezel is 1.6 x this
		float rgb[ 3 ] = { 0.0f, 0.0f, 0.0f };
	};

	/// Draw into the host's framebuffer, over what is there.
	bool Draw( GLuint hostFBO, const GLint viewport[ 4 ], sing::Lamp state, bool blinkOn );
	void DeInitGL();
	const Placement& Last() const
	{
		return last;
	}

	/// The lamp's colour for a state, in display values; unlit grey for a
	/// blinking lamp in its dark half.
	static void Colour( sing::Lamp state, bool blinkOn, float rgb[ 3 ] );
	/// The radius and centre for a viewport (x, y, w, h), window pixels.
	static Placement Place( const GLint viewport[ 4 ] );

private:
	bool Ensure();

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
	bool tried = false, ready = false;
	Placement last;
};
} // namespace flyback
