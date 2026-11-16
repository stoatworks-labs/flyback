#include "earshot/testing.h"

// EARSHOT_CAPTURE=OFF: no device backend. selectDevice() reports Failed with
// "this build has no capture backend"; feed() and the host FFT still work.
namespace earshot
{
namespace testing
{
std::unique_ptr< CaptureBackend > makeMiniaudioBackend() noexcept
{
	return nullptr;
}
} // namespace testing
} // namespace earshot
