#pragma once

// Where an instance is inside a host call, written somewhere that outlives the
// process.
//
// A plugin cannot catch a crash. Installing a signal handler or an exception
// filter inside Resolume would replace the host's own, for every plugin in it,
// and is not a plugin's decision to make. What a plugin CAN do is leave a note
// before each risky step and tidy it up after, so that if the process dies
// between the two the note is still there on the next load. That note is this.
//
// The slot lives in a memory-mapped file (see src/licence/CrashMarks.h). A
// write here is an ordinary store to memory — no system call, no lock, nothing
// that can stall a frame — and because the memory belongs to a file mapping,
// the operating system keeps it and writes it back after the process is gone.
// Nothing on this path allocates or throws.

#include <cstddef>
#include <cstdint>

namespace datamosh {

/// One instance's record in the marker file. The layout is part of the file
/// format: change it and bump CrashMarks' LAYOUT.
struct BreadcrumbSlot
{
	std::uint32_t claimed;   ///< 1 while an instance owns this slot
	std::uint32_t inFrame;   ///< 1 from entering a host call that renders until leaving it
	std::uint64_t frames;    ///< host calls that rendered, for "how long had it been running"
	std::uint32_t width;     ///< the last frame's size
	std::uint32_t height;
	char          stage[ 40 ];  ///< the step it was on, NUL-terminated
};

/// Writes one instance's slot. Unbound — no marker file, or tests that never
/// asked for one — every call is a no-op.
class Breadcrumb
{
public:
	void Bind( BreadcrumbSlot* target ) noexcept { slot = target; }
	BreadcrumbSlot* Bound() const noexcept { return const_cast< BreadcrumbSlot* >( slot ); }

	/// Entering a host call that can crash inside our code or the driver.
	void Enter( const char* stage ) noexcept
	{
		if( !slot )
			return;
		Stage( stage );
		slot->inFrame = 1;
	}

	void Stage( const char* stage ) noexcept
	{
		if( !slot )
			return;
		// Byte by byte through the volatile pointer, so the compiler cannot
		// defer or drop a store it sees nothing in this process read back.
		std::size_t index = 0;
		if( stage )
			for( ; index + 1 < sizeof( slot->stage ) && stage[ index ] != '\0'; ++index )
				slot->stage[ index ] = stage[ index ];
		slot->stage[ index ] = '\0';
	}

	void Rendered( std::uint32_t width, std::uint32_t height ) noexcept
	{
		if( !slot )
			return;
		slot->frames = slot->frames + 1;
		slot->width  = width;
		slot->height = height;
	}

	/// Left the call cleanly: a death after this is not ours.
	void Leave() noexcept
	{
		if( slot )
			slot->inFrame = 0;
	}

private:
	volatile BreadcrumbSlot* slot = nullptr;
};

/// Enter on construction, Leave on every way out, including an exception.
class ScopedBreadcrumb
{
public:
	ScopedBreadcrumb( Breadcrumb& breadcrumb, const char* stage ) noexcept :
		breadcrumb( breadcrumb )
	{
		breadcrumb.Enter( stage );
	}
	~ScopedBreadcrumb() { breadcrumb.Leave(); }

	ScopedBreadcrumb( const ScopedBreadcrumb& )            = delete;
	ScopedBreadcrumb& operator=( const ScopedBreadcrumb& ) = delete;

private:
	Breadcrumb& breadcrumb;
};

}  // namespace datamosh
