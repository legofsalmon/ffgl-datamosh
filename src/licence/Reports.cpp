#include "Reports.h"

#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace datamosh::licence::reports {

namespace fs = std::filesystem;

namespace {

std::string ReadAll( const fs::path& path )
{
	std::ifstream file( path, std::ios::binary );
	if( !file )
		return {};
	std::ostringstream contents;
	contents << file.rdbuf();
	return contents.str();
}

/// Written to a temporary name and renamed, so a reader in the other plugin
/// binary never sees half a report.
bool WriteAll( const fs::path& path, const std::string& contents )
{
	std::error_code error;
	fs::create_directories( path.parent_path(), error );
	fs::path temporary = path;
	temporary += ".tmp";
	{
		std::ofstream file( temporary, std::ios::binary | std::ios::trunc );
		if( !file )
			return false;
		file << contents;
		if( !file )
			return false;
	}
	fs::rename( temporary, path, error );
	if( error )
	{
		fs::remove( temporary, error );
		return false;
	}
	return true;
}

std::string Trim( std::string text )
{
	while( !text.empty() && std::isspace( static_cast< unsigned char >( text.back() ) ) )
		text.pop_back();
	size_t start = 0;
	while( start < text.size() && std::isspace( static_cast< unsigned char >( text[ start ] ) ) )
		++start;
	return text.substr( start );
}

void ReplaceAll( std::string& text, const std::string& from, const std::string& to )
{
	if( from.empty() )
		return;
	size_t position = 0;
	while( ( position = text.find( from, position ) ) != std::string::npos )
	{
		text.replace( position, from.size(), to );
		position += to.size();
	}
}

bool IsNameChar( char c )
{
	return std::isalnum( static_cast< unsigned char >( c ) ) || c == '.' || c == '_' || c == '-';
}

/// Replaces the path segment after each `marker` ("/Users/") with <user>.
void ScrubAfter( std::string& text, const std::string& marker, bool caseInsensitive )
{
	auto find = [ & ]( size_t from ) {
		if( !caseInsensitive )
			return text.find( marker, from );
		for( size_t i = from; i + marker.size() <= text.size(); ++i )
		{
			bool match = true;
			for( size_t j = 0; j < marker.size() && match; ++j )
				match = std::tolower( static_cast< unsigned char >( text[ i + j ] ) ) ==
				        std::tolower( static_cast< unsigned char >( marker[ j ] ) );
			if( match )
				return i;
		}
		return std::string::npos;
	};

	size_t position = 0;
	while( ( position = find( position ) ) != std::string::npos )
	{
		const size_t start = position + marker.size();
		// A user folder can have spaces in it (C:\Users\Colm Hewson\AppData), so
		// the segment runs to the next separator when there is one on this
		// line. Only at the end of a path does it stop at a space instead.
		// Taking too much is safe; leaving half a name behind is not.
		auto stopsAt = [ & ]( size_t i, bool spaces ) {
			const char c = text[ i ];
			return c == '/' || c == '\\' || c == '"' || c == '\'' || c == '\n' || c == '\r' || c == '\t' ||
			       ( spaces && c == ' ' );
		};
		size_t end = start;
		while( end < text.size() && !stopsAt( end, false ) )
			++end;
		if( end >= text.size() || ( text[ end ] != '/' && text[ end ] != '\\' ) )
		{
			end = start;
			while( end < text.size() && !stopsAt( end, true ) )
				++end;
		}
		if( end > start && text.compare( start, end - start, "<user>" ) != 0 )
			text.replace( start, end - start, "<user>" );
		position = start + 6;
	}
}

/// The user name as a whole word only, so a short name cannot eat the
/// middle of an ordinary word.
void ScrubWord( std::string& text, const std::string& word )
{
	if( word.size() < 2 )
		return;
	size_t position = 0;
	while( ( position = text.find( word, position ) ) != std::string::npos )
	{
		const bool before = position > 0 && IsNameChar( text[ position - 1 ] );
		const bool after  = position + word.size() < text.size() && IsNameChar( text[ position + word.size() ] );
		if( before || after )
		{
			position += word.size();
			continue;
		}
		text.replace( position, word.size(), "<user>" );
		position += 6;
	}
}

std::uint64_t Fnv1a( const std::string& text )
{
	std::uint64_t hash = 1469598103934665603ULL;
	for( const char c : text )
	{
		hash ^= static_cast< unsigned char >( c );
		hash *= 1099511628211ULL;
	}
	return hash;
}

/// Which process a queued or pending file came from: the number its name
/// starts with.
std::uint32_t PidOf( const fs::path& path )
{
	const std::string name = path.filename().u8string();
	std::uint32_t     pid  = 0;
	size_t            i    = 0;
	for( ; i < name.size() && std::isdigit( static_cast< unsigned char >( name[ i ] ) ); ++i )
		pid = pid * 10 + static_cast< std::uint32_t >( name[ i ] - '0' );
	return ( i > 0 && i < name.size() && name[ i ] == '-' ) ? pid : 0;
}

const char* SENDING = ".sending";

}  // namespace

const char* OsName()
{
#if defined( _WIN32 )
	return "windows";
#elif defined( __APPLE__ )
	return "macos";
#else
	return "linux";
#endif
}

const char* Arch()
{
#if defined( __aarch64__ ) || defined( _M_ARM64 )
	return "arm64";
#elif defined( __x86_64__ ) || defined( _M_X64 )
	return "x86_64";
#else
	return "unknown";
#endif
}

std::string UserAgent( const std::string& version )
{
	return "Datamosh/" + version + " (" + OsName() + ")";
}

std::string FeedbackUrl( const std::string& base, const std::string& version )
{
	// Both values are ours and URL-safe already ("datamosh", "1.0.0"), but a
	// build with a suffix ("1.0.0+local") should not produce a broken link.
	std::string encoded;
	for( const char c : version )
	{
		if( std::isalnum( static_cast< unsigned char >( c ) ) || c == '.' || c == '-' || c == '_' || c == '~' )
			encoded.push_back( c );
		else
		{
			char buffer[ 4 ];
			std::snprintf( buffer, sizeof( buffer ), "%%%02X", static_cast< unsigned char >( c ) );
			encoded += buffer;
		}
	}
	return base + FEEDBACK_PAGE + "?product=" + PRODUCT + "&version=" + encoded;
}

std::string Truncate( const std::string& text, size_t limit )
{
	if( text.size() <= limit )
		return text;
	size_t cut = limit;
	// Back up over continuation bytes to the start of the sequence.
	while( cut > 0 && ( static_cast< unsigned char >( text[ cut ] ) & 0xC0 ) == 0x80 )
		--cut;
	return text.substr( 0, cut );
}

std::string Scrub( const std::string& input, const std::string& home, const std::string& user )
{
	std::string text = input;

	// The home folder first, in both slash styles, so what is left after it
	// reads as "~/Library/..." rather than "<user>/Library/...".
	if( home.size() > 1 )
	{
		std::string trimmed = home;
		while( trimmed.size() > 1 && ( trimmed.back() == '/' || trimmed.back() == '\\' ) )
			trimmed.pop_back();
		std::string other = trimmed;
		for( char& c : other )
			c = c == '\\' ? '/' : ( c == '/' ? '\\' : c );
		ReplaceAll( text, trimmed, "~" );
		ReplaceAll( text, other, "~" );
	}

	// Anyone's home folder, not only this one's: a path handed over by the
	// host, or a second account on the machine.
	ScrubAfter( text, "/Users/", false );
	ScrubAfter( text, "/home/", false );
	ScrubAfter( text, ":\\Users\\", true );
	ScrubAfter( text, ":/Users/", true );
	ScrubAfter( text, "\\\\Users\\", true );

	ScrubWord( text, user );

	// A query string can carry anything — a token, an email, a key.
	for( const char* scheme : { "http://", "https://" } )
	{
		size_t position = 0;
		while( ( position = text.find( scheme, position ) ) != std::string::npos )
		{
			size_t end = position;
			while( end < text.size() && !std::isspace( static_cast< unsigned char >( text[ end ] ) ) &&
			       text[ end ] != '"' && text[ end ] != '\'' )
				++end;
			const size_t question = text.find( '?', position );
			if( question != std::string::npos && question < end )
				text.erase( question, end - question );
			position += std::char_traits< char >::length( scheme );
		}
	}
	return text;
}

std::string Signature( const std::string& kind, const std::string& binary, const std::string& where )
{
	char buffer[ 17 ];
	std::snprintf( buffer, sizeof( buffer ), "%016llx",
	               static_cast< unsigned long long >( Fnv1a( kind + "|" + binary + "|" + where ) ) );
	return std::string( buffer, 12 );
}

std::string NewInstallId()
{
	std::random_device                              device;
	std::uniform_int_distribution< unsigned int >   byte( 0, 255 );
	unsigned char                                   bytes[ 16 ];
	for( unsigned char& b : bytes )
		b = static_cast< unsigned char >( byte( device ) );
	bytes[ 6 ] = static_cast< unsigned char >( ( bytes[ 6 ] & 0x0F ) | 0x40 );  // version 4
	bytes[ 8 ] = static_cast< unsigned char >( ( bytes[ 8 ] & 0x3F ) | 0x80 );  // RFC 4122 variant

	char out[ 37 ];
	std::snprintf( out, sizeof( out ), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	               bytes[ 0 ], bytes[ 1 ], bytes[ 2 ], bytes[ 3 ], bytes[ 4 ], bytes[ 5 ], bytes[ 6 ], bytes[ 7 ],
	               bytes[ 8 ], bytes[ 9 ], bytes[ 10 ], bytes[ 11 ], bytes[ 12 ], bytes[ 13 ], bytes[ 14 ],
	               bytes[ 15 ] );
	return out;
}

bool ValidInstallId( const std::string& id )
{
	if( id.empty() || id.size() > MAX_INSTALL )
		return false;
	return std::all_of( id.begin(), id.end(),
	                    []( char c ) { return std::isalnum( static_cast< unsigned char >( c ) ) || c == '-'; } );
}

std::string CrashPayload( const CrashReport& report )
{
	std::string body = "{";
	bool        first = true;
	auto add = [ & ]( const char* name, const std::string& value, size_t limit, bool required ) {
		const std::string cut = Truncate( value, limit );
		if( cut.empty() && !required )
			return;
		body += ( first ? "" : "," );
		body += json::Quote( name ) + ":" + json::Quote( cut );
		first = false;
	};

	add( "product", PRODUCT, 32, true );
	add( "version", report.version, MAX_VERSION, true );
	add( "os", report.os, 16, true );
	add( "osVersion", report.osVersion, MAX_OS_VERSION, false );
	add( "arch", report.arch, MAX_ARCH, false );
	add( "install", ValidInstallId( report.install ) ? report.install : std::string(), MAX_INSTALL, false );
	add( "kind", report.kind, 32, true );
	add( "summary", report.summary.empty() ? std::string( "(no summary)" ) : report.summary, MAX_SUMMARY, true );
	add( "signature", report.signature, MAX_SIGNATURE, false );
	add( "occurredAt", report.occurredAt, 40, false );
	// Last, and cut so the whole body fits the contract's 64 KB even when
	// every other field is at its limit and escaping has grown the detail.
	std::string detail = Truncate( report.detail, MAX_DETAIL );
	while( !detail.empty() && body.size() + json::Quote( detail ).size() + 16 > MAX_BODY )
		detail = Truncate( detail, detail.size() / 2 );
	add( "detail", detail, MAX_DETAIL, false );
	body += "}";
	return body;
}

Outcome Classify( const HttpResponse& response )
{
	if( !response.reached )
		return Outcome::Keep;
	if( response.status >= 200 && response.status < 300 )
		return Outcome::Sent;
	if( response.status == 400 || response.status == 413 )
		return Outcome::Drop;
	return Outcome::Keep;
}

std::string IsoTime( std::int64_t unixSeconds )
{
	const std::time_t seconds = static_cast< std::time_t >( unixSeconds );
	std::tm           utc{};
#if defined( _WIN32 )
	gmtime_s( &utc, &seconds );
#else
	gmtime_r( &seconds, &utc );
#endif
	char buffer[ 32 ];
	std::strftime( buffer, sizeof( buffer ), "%Y-%m-%dT%H:%M:%SZ", &utc );
	return buffer;
}

// ---------------------------------------------------------------------------

Queue::Queue( fs::path folder ) :
	folder( std::move( folder ) )
{
}

bool Queue::Add( const std::string& json, const std::string& stem )
{
	if( folder.empty() )
		return false;
	if( !WriteAll( folder / ( stem + ".json" ), json ) )
		return false;
	auto files = Files();
	std::error_code error;
	while( files.size() > QUEUE_LIMIT )
	{
		fs::remove( files.front(), error );
		files.erase( files.begin() );
	}
	return true;
}

std::vector< fs::path > Queue::Files() const
{
	std::vector< std::pair< fs::file_time_type, fs::path > > found;
	std::error_code error;
	if( folder.empty() )
		return {};
	for( fs::directory_iterator it( folder, error ), end; !error && it != end; it.increment( error ) )
	{
		if( it->path().extension() != ".json" )
			continue;
		std::error_code timeError;
		found.emplace_back( fs::last_write_time( it->path(), timeError ), it->path() );
	}
	// Oldest first; the name breaks ties, and it carries a sequence number.
	std::sort( found.begin(), found.end(), []( const auto& a, const auto& b ) {
		return a.first != b.first ? a.first < b.first : a.second.filename() < b.second.filename();
	} );
	std::vector< fs::path > paths;
	for( auto& entry : found )
		paths.push_back( std::move( entry.second ) );
	return paths;
}

void Queue::Clear() const
{
	std::error_code error;
	for( const fs::path& path : Files() )
		fs::remove( path, error );
}

// ---------------------------------------------------------------------------

Reporter::Reporter( Config config, Transport* transport, std::function< bool( const std::string& ) > openUrl ) :
	config( std::move( config ) ),
	transport( transport ),
	openUrl( std::move( openUrl ) ),
	queue( this->config.folder.empty() ? fs::path() : this->config.folder / "queue" ),
	pending( this->config.folder.empty() ? fs::path() : this->config.folder / "pending" )
{
}

fs::path Reporter::SettingsFile() const
{
	return config.folder / "settings.txt";
}

bool Reporter::Automatic() const
{
	if( !Enabled() )
		return false;
	const std::string text = ReadAll( SettingsFile() );
	return text.find( "send-automatically=yes" ) != std::string::npos;
}

std::string Reporter::Stem( const std::string& kind )
{
	// The process id first: it is how a later launch tells a question it
	// already asked from one another running process is still asking.
	return std::to_string( config.self ) + "-" + config.binary + "-" + kind + "-" +
	       std::to_string( config.now ? config.now() : 0 ) + "-" + std::to_string( ++sequence );
}

std::string Reporter::Scrubbed( const std::string& text ) const
{
	return Scrub( text, config.home, config.user );
}

std::string Reporter::DetailLines( const std::vector< std::pair< std::string, std::string > >& lines ) const
{
	std::string detail;
	for( const auto& line : lines )
		if( !line.second.empty() )
			detail += line.first + ": " + line.second + "\n";
	return Scrubbed( detail );
}

void Reporter::Start()
{
	if( !Enabled() )
		return;
	std::error_code error;
	fs::create_directories( config.folder, error );

	// The install id: made once, random, kept beside the licence.
	installId = Trim( ReadAll( config.installIdFile ) );
	if( !ValidInstallId( installId ) )
	{
		installId = NewInstallId();
		WriteAll( config.installIdFile, installId + "\n" );
	}

	// Asked once. A question from a launch that has ended was not answered,
	// and "not answered" is "don't send". Another Resolume still running
	// keeps its own.
	for( const fs::path& path : pending.Files() )
	{
		const std::uint32_t pid = PidOf( path );
		if( pid != config.self && !( config.alive && config.alive( pid ) ) )
			fs::remove( path, error );
	}

	// A send that died with its process goes back in the queue.
	for( fs::directory_iterator it( queue.Folder(), error ), end; !error && it != end; it.increment( error ) )
	{
		const std::string name = it->path().filename().u8string();
		const size_t      mark = name.find( SENDING );
		if( mark == std::string::npos )
			continue;
		const std::string owner = name.substr( mark + std::char_traits< char >::length( SENDING ) + 1 );
		std::uint32_t     pid   = 0;
		for( const char c : owner )
			if( std::isdigit( static_cast< unsigned char >( c ) ) )
				pid = pid * 10 + static_cast< std::uint32_t >( c - '0' );
			else
				break;
		if( pid == config.self || ( config.alive && config.alive( pid ) ) )
			continue;
		std::error_code renameError;
		fs::rename( it->path(), queue.Folder() / name.substr( 0, mark ), renameError );
	}

	for( const crash::Finding& finding :
	     crash::Harvest( config.folder, config.binary, config.alive, config.self ) )
	{
		const bool mixer = finding.binary.find( "Transplant" ) != std::string::npos;
		CrashReport report;
		report.version   = finding.version.empty() ? config.version : finding.version;
		report.os        = OsName();
		report.osVersion = config.osVersion;
		report.arch      = Arch();
		report.install   = installId;
		report.kind      = "unclean-exit";
		report.summary   = Truncate( Scrubbed( "The host closed while " + finding.binary + " was inside a call (" +
		                                       ( finding.stage.empty() ? std::string( "unknown stage" )
		                                                               : finding.stage ) +
		                                       ")" ),
		                             MAX_SUMMARY );
		report.detail    = DetailLines( {
			{ "plugin", finding.binary + ( mixer ? " (blend mode)" : " (effect)" ) + " " + finding.version },
			{ "host", finding.host },
			{ "stage", finding.stage },
			{ "frames rendered by this instance", std::to_string( finding.frames ) },
			{ "frame size", finding.width ? std::to_string( finding.width ) + "x" + std::to_string( finding.height )
			                              : std::string() },
			{ "instances open", std::to_string( finding.instances ) },
			{ "how this was found",
			  "the plugin's crash marker was still set when the next session started; no stack trace is "
			  "collected inside a host application" },
		} );
		report.signature = Signature( report.kind, finding.binary, finding.stage );
		Record( report, Stem( report.kind ), true );
	}

	// Only the queue is scheduled. What is pending stays pending until the
	// person answers or the setting is on; Record already put everything
	// found above in the right one.
	sendAfter = ( config.now ? config.now() : 0 ) + SEND_DELAY;
}

void Reporter::Record( const CrashReport& report, const std::string& stem, bool crash )
{
	if( !Enabled() )
		return;
	const std::string json = CrashPayload( report );
	if( Automatic() )
		queue.Add( json, stem );
	else if( pending.Add( json, stem ) && crash )
		askedAboutCrash = true;
}

void Reporter::Caught( const std::string& where, const std::string& what )
{
	if( !Enabled() )
		return;
	const std::string key = where + "|" + what;
	if( !seenThisProcess.insert( key ).second )
		return;

	CrashReport report;
	report.version    = config.version;
	report.os         = OsName();
	report.osVersion  = config.osVersion;
	report.arch       = Arch();
	report.install    = installId;
	report.kind       = "exception";
	report.summary    = Truncate( Scrubbed( "Caught in " + where + ": " + what ), MAX_SUMMARY );
	report.detail     = DetailLines( {
		{ "plugin", config.binary + " " + config.version },
		{ "where", where },
		{ "what", what },
		{ "what happened next", "the frame was passed through untouched and the plugin carried on" },
	} );
	report.signature  = Signature( report.kind, config.binary, where + "|" + Scrubbed( what ) );
	report.occurredAt = IsoTime( config.now ? config.now() : 0 );
	Record( report, Stem( report.kind ), false );
}

bool Reporter::Asking() const
{
	for( const fs::path& path : pending.Files() )
		if( PidOf( path ) == config.self )
			return true;
	return false;
}

bool Reporter::AskingAboutACrash() const
{
	for( const fs::path& path : pending.Files() )
		if( PidOf( path ) == config.self && path.filename().u8string().find( "-unclean-exit-" ) != std::string::npos )
			return true;
	return false;
}

void Reporter::Promote()
{
	std::error_code error;
	for( const fs::path& path : pending.Files() )
	{
		if( PidOf( path ) != config.self )
			continue;
		const std::string json = ReadAll( path );
		if( !json.empty() )
			queue.Add( json, path.stem().u8string() );
		fs::remove( path, error );
	}
}

void Reporter::Send()
{
	if( !Enabled() )
		return;
	Promote();
	sendAfter    = config.now ? config.now() : 0;
	flushBlocked = false;
	notice       = "sending...";
}

void Reporter::Discard()
{
	std::error_code error;
	for( const fs::path& path : pending.Files() )
		if( PidOf( path ) == config.self )
			fs::remove( path, error );
	notice = "not sent";
}

void Reporter::SetAutomatic( bool on )
{
	if( !Enabled() )
	{
		notice = "no folder for crash reports on this computer";
		return;
	}
	WriteAll( SettingsFile(), std::string( "send-automatically=" ) + ( on ? "yes" : "no" ) + "\n" );
	if( on )
	{
		Send();
		notice = "crash reports on";
	}
	else
	{
		// Off means off: nothing already waiting goes either.
		queue.Clear();
		Discard();
		notice = "crash reports off";
	}
}

bool Reporter::OpenFeedback()
{
	const std::int64_t now = config.now ? config.now() : 0;
	if( now - lastFeedback < FEEDBACK_DEBOUNCE )
		return true;
	lastFeedback      = now;
	const std::string url = FeedbackUrl( config.serviceBase, config.version );
	if( openUrl && openUrl( url ) )
		return true;
	notice = "could not open a browser - the address is in README.txt";
	return false;
}

void Reporter::Pump()
{
	if( !Enabled() )
		return;
	if( Automatic() )
		Promote();
	const std::int64_t now = config.now ? config.now() : 0;
	if( flushBlocked || now < sendAfter )
		return;
	Flush();
}

void Reporter::Flush()
{
	if( !transport )
		return;
	std::error_code error;
	const std::string owner = std::string( SENDING ) + "-" + std::to_string( config.self ) + "-" + config.binary;
	size_t sent = 0;
	for( const fs::path& path : queue.Files() )
	{
		// Claimed by renaming, so the other plugin binary — which shares this
		// folder — cannot send the same report at the same moment.
		fs::path claimed = path;
		claimed += owner;
		fs::rename( path, claimed, error );
		if( error )
		{
			error.clear();
			continue;
		}

		const std::string json = ReadAll( claimed );
		if( json.empty() )
		{
			fs::remove( claimed, error );
			continue;
		}

		PostOptions options;
		options.timeoutSeconds = TIMEOUT_SECONDS;
		options.userAgent      = UserAgent( config.version );
		const HttpResponse response = transport->Post( CRASH_PATH, json, options );

		switch( Classify( response ) )
		{
		case Outcome::Sent:
			++sent;
			[[fallthrough]];
		case Outcome::Drop:
			fs::remove( claimed, error );
			break;
		case Outcome::Keep:
			// Offline, rate-limited or a server error: it stays queued, and the
			// next launch tries again. Never a retry loop inside the host.
			fs::rename( claimed, path, error );
			flushBlocked = true;
			if( notice == "sending..." )
				notice = "not sent yet - it will go when the site can be reached";
			return;
		}
	}
	if( notice == "sending..." )
		notice = sent == 1 ? "report sent, thank you" : "reports sent, thank you";
}

std::string Reporter::Describe() const
{
	std::ostringstream out;
	out << "Crash reports and feedback\n"
	    << "--------------------------\n\n";
	if( !Enabled() )
	{
		out << "Not available: there is no folder for them on this computer.\n";
		return out.str();
	}
	out << "Send crash reports automatically: " << ( Automatic() ? "on" : "off" ) << "\n";
	if( Asking() )
		out << "\n"
		    << ( AskingAboutACrash() ? "Datamosh closed unexpectedly last time."
		                             : "Datamosh caught a problem and carried on." )
		    << " Send a crash report to LeTissier\n"
		    << "Creative Studios? Type \"send\" or \"discard\" into the Licence field, or\n"
		    << "\"always send\" to send this one and every one after it.\n";
	const size_t queued = queue.Size();
	if( queued > 0 )
		out << "Waiting to be sent: " << queued << ( queued == 1 ? " report" : " reports" ) << "\n";
	out << "\n"
	    << "Type into the Licence field:\n\n"
	    << "  always send     turns automatic crash reports on (reports on)\n"
	    << "  reports off     turns them off, and discards anything not yet sent\n"
	    << "  send / discard  answers the question after a problem\n"
	    << "  feedback        opens the feedback page (so does the Send Feedback button)\n\n"
	    << "A crash report holds: the product and its version, the operating system and\n"
	    << "its version, the processor type, a random install id made on this computer\n"
	    << "(" << ( installId.empty() ? std::string( "not made yet" ) : installId ) << ", from nothing about you or it), what\n"
	    << "kind of problem it was, a one-line summary, which step of the frame it was on,\n"
	    << "the Resolume version, the frame size and how many frames had been drawn.\n"
	    << "Home folders and user names are removed before anything is saved.\n\n"
	    << "It never holds your licence key, your email, your name, file names, the\n"
	    << "contents of a composition, video, audio, or anything you typed.\n\n"
	    << "Reports go to " << config.serviceBase << CRASH_PATH << " and nowhere else.\n"
	    << "Feedback: " << FeedbackUrl( config.serviceBase, config.version ) << "\n";
	return out.str();
}

}  // namespace datamosh::licence::reports
