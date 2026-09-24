#include "CrashMarks.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace datamosh::licence::crash {

namespace fs = std::filesystem;

namespace {

std::mutex                         openMutex;
std::atomic< MarkFile* >           mapped{ nullptr };
std::atomic< bool >                taken[ SLOTS ];
fs::path                           mappedPath;

#if defined( _WIN32 )
HANDLE fileHandle    = INVALID_HANDLE_VALUE;
HANDLE mappingHandle = nullptr;
#else
int fileDescriptor = -1;
#endif

template< size_t N >
void CopyField( char ( &field )[ N ], const char* text )
{
	size_t index = 0;
	if( text )
		for( ; index + 1 < N && text[ index ] != '\0'; ++index )
			field[ index ] = text[ index ];
	for( ; index < N; ++index )
		field[ index ] = '\0';
}

template< size_t N >
std::string ReadField( const char ( &field )[ N ] )
{
	size_t length = 0;
	while( length < N && field[ length ] != '\0' )
		++length;
	return std::string( field, length );
}

MarkFile* MapFile( const fs::path& path )
{
#if defined( _WIN32 )
	// Shared for read, write and delete, so the next load can read and remove
	// it once this process is gone.
	HANDLE file = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
	                           FILE_ATTRIBUTE_NORMAL, nullptr );
	if( file == INVALID_HANDLE_VALUE )
		return nullptr;
	HANDLE mapping = CreateFileMappingW( file, nullptr, PAGE_READWRITE, 0, static_cast< DWORD >( sizeof( MarkFile ) ),
	                                     nullptr );
	if( mapping == nullptr )
	{
		CloseHandle( file );
		return nullptr;
	}
	void* view = MapViewOfFile( mapping, FILE_MAP_WRITE, 0, 0, sizeof( MarkFile ) );
	if( view == nullptr )
	{
		CloseHandle( mapping );
		CloseHandle( file );
		return nullptr;
	}
	fileHandle    = file;
	mappingHandle = mapping;
	return static_cast< MarkFile* >( view );
#else
	const int descriptor = ::open( path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600 );
	if( descriptor < 0 )
		return nullptr;
	if( ::ftruncate( descriptor, static_cast< off_t >( sizeof( MarkFile ) ) ) != 0 )
	{
		::close( descriptor );
		return nullptr;
	}
	void* view = ::mmap( nullptr, sizeof( MarkFile ), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0 );
	if( view == MAP_FAILED )
	{
		::close( descriptor );
		return nullptr;
	}
	fileDescriptor = descriptor;
	return static_cast< MarkFile* >( view );
#endif
}

void UnmapFile( MarkFile* file )
{
	if( file == nullptr )
		return;
#if defined( _WIN32 )
	UnmapViewOfFile( file );
	if( mappingHandle )
		CloseHandle( mappingHandle );
	if( fileHandle != INVALID_HANDLE_VALUE )
		CloseHandle( fileHandle );
	mappingHandle = nullptr;
	fileHandle    = INVALID_HANDLE_VALUE;
#else
	::munmap( file, sizeof( MarkFile ) );
	if( fileDescriptor >= 0 )
		::close( fileDescriptor );
	fileDescriptor = -1;
#endif
}

bool ReadMarker( const fs::path& path, MarkFile& out )
{
	std::ifstream file( path, std::ios::binary );
	if( !file )
		return false;
	file.read( reinterpret_cast< char* >( &out ), sizeof( MarkFile ) );
	if( file.gcount() != static_cast< std::streamsize >( sizeof( MarkFile ) ) )
		return false;
	return std::memcmp( out.magic, MAGIC, sizeof( MAGIC ) ) == 0 && out.layout == LAYOUT;
}

}  // namespace

fs::path RunningFolder( const fs::path& reportsFolder )
{
	return reportsFolder / "running";
}

std::uint32_t CurrentPid()
{
#if defined( _WIN32 )
	return static_cast< std::uint32_t >( GetCurrentProcessId() );
#else
	return static_cast< std::uint32_t >( ::getpid() );
#endif
}

bool ProcessAlive( std::uint32_t pid )
{
	if( pid == 0 )
		return false;
#if defined( _WIN32 )
	HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast< DWORD >( pid ) );
	if( process == nullptr )
		// Access denied means it exists; anything else, that it does not.
		return GetLastError() == ERROR_ACCESS_DENIED;
	DWORD code = 0;
	const BOOL ok = GetExitCodeProcess( process, &code );
	CloseHandle( process );
	return !ok || code == STILL_ACTIVE;
#else
	if( ::kill( static_cast< pid_t >( pid ), 0 ) == 0 )
		return true;
	return errno == EPERM;
#endif
}

bool Open( const fs::path& reportsFolder, const char* binary, const char* version )
{
	if( mapped.load( std::memory_order_acquire ) )
		return true;
	if( reportsFolder.empty() )
		return false;

	std::lock_guard< std::mutex > lock( openMutex );
	if( mapped.load( std::memory_order_acquire ) )
		return true;

	std::error_code error;
	const fs::path  folder = RunningFolder( reportsFolder );
	fs::create_directories( folder, error );

	const fs::path path =
		folder / ( std::string( binary ? binary : "plugin" ) + "-" + std::to_string( CurrentPid() ) + ".mark" );
	MarkFile* file = MapFile( path );
	if( file == nullptr )
		return false;

	std::memset( static_cast< void* >( file ), 0, sizeof( MarkFile ) );
	std::memcpy( file->magic, MAGIC, sizeof( MAGIC ) );
	file->layout    = LAYOUT;
	file->pid       = CurrentPid();
	file->startedAt = std::chrono::duration_cast< std::chrono::seconds >(
	                      std::chrono::system_clock::now().time_since_epoch() )
	                      .count();
	CopyField( file->binary, binary );
	CopyField( file->version, version );

	for( auto& flag : taken )
		flag.store( false, std::memory_order_relaxed );
	mappedPath = path;
	mapped.store( file, std::memory_order_release );
	return true;
}

bool IsOpen()
{
	return mapped.load( std::memory_order_acquire ) != nullptr;
}

BreadcrumbSlot* Claim()
{
	MarkFile* file = mapped.load( std::memory_order_acquire );
	if( file == nullptr )
		return nullptr;
	for( int index = 0; index < SLOTS; ++index )
	{
		if( !taken[ index ].exchange( true, std::memory_order_acq_rel ) )
		{
			volatile BreadcrumbSlot& slot = file->slots[ index ];
			slot.inFrame = 0;
			slot.frames  = 0;
			slot.width   = 0;
			slot.height  = 0;
			slot.stage[ 0 ] = '\0';
			slot.claimed = 1;
			return &file->slots[ index ];
		}
	}
	return nullptr;
}

void Release( BreadcrumbSlot* slot )
{
	MarkFile* file = mapped.load( std::memory_order_acquire );
	if( file == nullptr || slot == nullptr )
		return;
	const std::ptrdiff_t index = slot - file->slots;
	if( index < 0 || index >= SLOTS )
		return;
	volatile BreadcrumbSlot& target = *slot;
	target.inFrame = 0;
	target.claimed = 0;
	taken[ index ].store( false, std::memory_order_release );
}

void SetHost( const char* name, const char* version )
{
	MarkFile* file = mapped.load( std::memory_order_acquire );
	if( file == nullptr )
		return;
	std::string text = name ? name : "";
	if( version && *version )
		text += std::string( text.empty() ? "" : " " ) + version;
	CopyField( file->host, text.c_str() );
}

std::vector< Finding > Harvest( const fs::path& reportsFolder, const std::string& binary,
                                const std::function< bool( std::uint32_t ) >& alive, std::uint32_t self )
{
	std::vector< Finding > findings;
	if( reportsFolder.empty() )
		return findings;

	std::error_code error;
	const fs::path  folder = RunningFolder( reportsFolder );
	const std::string prefix = binary + "-";
	for( fs::directory_iterator it( folder, error ), end; !error && it != end; it.increment( error ) )
	{
		const fs::path    path = it->path();
		const std::string name = path.filename().u8string();
		if( name.rfind( prefix, 0 ) != 0 || path.extension() != ".mark" )
			continue;

		MarkFile marker{};
		if( !ReadMarker( path, marker ) )
		{
			// Not a marker this build can read — torn, or from another layout.
			// Nothing can be learned from it; it goes.
			std::error_code ignored;
			fs::remove( path, ignored );
			continue;
		}
		if( marker.pid == self || ( alive && alive( marker.pid ) ) )
			continue;

		int instances = 0;
		for( const BreadcrumbSlot& slot : marker.slots )
			instances += slot.claimed ? 1 : 0;

		for( const BreadcrumbSlot& slot : marker.slots )
		{
			if( !slot.inFrame )
				continue;
			Finding finding;
			finding.binary    = ReadField( marker.binary );
			finding.version   = ReadField( marker.version );
			finding.host      = ReadField( marker.host );
			finding.stage     = ReadField( slot.stage );
			finding.frames    = slot.frames;
			finding.width     = slot.width;
			finding.height    = slot.height;
			finding.pid       = marker.pid;
			finding.instances = instances;
			findings.push_back( std::move( finding ) );
		}

		std::error_code ignored;
		fs::remove( path, ignored );
	}
	return findings;
}

namespace testing {

void Close()
{
	std::lock_guard< std::mutex > lock( openMutex );
	MarkFile* file = mapped.exchange( nullptr, std::memory_order_acq_rel );
	UnmapFile( file );
	std::error_code ignored;
	if( !mappedPath.empty() )
		fs::remove( mappedPath, ignored );
	mappedPath.clear();
	for( auto& flag : taken )
		flag.store( false, std::memory_order_relaxed );
}

}  // namespace testing

}  // namespace datamosh::licence::crash
