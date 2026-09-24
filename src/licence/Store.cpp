#include "Store.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <sstream>

namespace datamosh::licence {

namespace fs = std::filesystem;

namespace {

std::string Trimmed( std::string text )
{
	while( !text.empty() && std::isspace( static_cast< unsigned char >( text.back() ) ) )
		text.pop_back();
	size_t start = 0;
	while( start < text.size() && std::isspace( static_cast< unsigned char >( text[ start ] ) ) )
		++start;
	return text.substr( start );
}

}  // namespace

Store::Store( fs::path directory ) :
	directory( std::move( directory ) )
{
}

bool Store::Ensure() const
{
	if( directory.empty() )
		return false;
	std::error_code error;
	fs::create_directories( directory, error );
	return fs::is_directory( directory, error );
}

std::optional< std::string > Store::Read( const char* name ) const
{
	if( directory.empty() )
		return std::nullopt;
	std::ifstream file( directory / name, std::ios::binary );
	if( !file )
		return std::nullopt;
	std::ostringstream contents;
	contents << file.rdbuf();
	std::string text = Trimmed( contents.str() );
	if( text.empty() )
		return std::nullopt;
	return text;
}

bool Store::Write( const char* name, const std::string& contents ) const
{
	if( !Ensure() )
		return false;

	// Unique per writer: both plugin binaries may write at the same moment,
	// and each has its own copy of this counter, so the address of the store
	// is folded in as well.
	static std::atomic< unsigned > counter{ 0 };
	const fs::path target    = directory / name;
	const fs::path temporary = directory / ( std::string( name ) + ".tmp" +
	                                         std::to_string( reinterpret_cast< std::uintptr_t >( this ) ) + "-" +
	                                         std::to_string( counter.fetch_add( 1 ) ) );
	{
		std::ofstream file( temporary, std::ios::binary | std::ios::trunc );
		if( !file )
			return false;
		file << contents;
		file.flush();
		if( !file )
		{
			file.close();
			std::error_code ignored;
			fs::remove( temporary, ignored );
			return false;
		}
	}

	std::error_code error;
	fs::rename( temporary, target, error );
	if( error )
	{
		std::error_code ignored;
		fs::remove( temporary, ignored );
		return false;
	}
	return true;
}

bool Store::RemoveDropFile() const
{
	std::error_code error;
	return fs::remove( directory / DROP_FILE, error );
}

void Store::Forget() const
{
	std::error_code error;
	fs::remove( directory / TOKEN_FILE, error );
	fs::remove( directory / KEY_FILE, error );
}

std::int64_t Store::LastCheckIn() const
{
	const auto text = Read( CHECK_IN_FILE );
	if( !text )
		return 0;
	return std::strtoll( text->c_str(), nullptr, 10 );
}

void Store::RecordCheckIn( std::int64_t when ) const
{
	Write( CHECK_IN_FILE, std::to_string( when ) + "\n" );
}

std::string Store::Stamp() const
{
	// By content rather than by modification time. Two tokens from the service
	// are the same length, and a filesystem with one-second timestamps would
	// otherwise miss the other binary's check-in when it lands in the same
	// second as the last read. These files are a few hundred bytes, and this
	// runs on the worker thread.
	std::string stamp;
	for( const char* name : { TOKEN_FILE, KEY_FILE, DROP_FILE } )
	{
		const auto contents = Read( name );
		stamp += contents ? std::to_string( std::hash< std::string >{}( *contents ) ) : std::string( "-" );
		stamp += "|";
	}
	return stamp;
}

}  // namespace datamosh::licence
