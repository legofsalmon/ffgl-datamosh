// Linux is not a Resolume platform. The plugins build here so CI can check
// their exports, and the test suite runs here, so this is the honest minimum:
// the machine id and a folder, and no HTTP client at all. Tests stand in for
// the network with their own Transport.

#include "Platform.h"

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace datamosh::licence {

namespace {

class NoTransport : public Transport
{
public:
	HttpResponse Post( const std::string&, const std::string& ) override
	{
		HttpResponse response;
		response.error = "this build has no HTTP client (Linux is a test platform only)";
		return response;
	}
};

std::string ReadMachineId()
{
	for( const char* path : { "/etc/machine-id", "/var/lib/dbus/machine-id" } )
	{
		std::ifstream file( path );
		std::string   id;
		if( file && std::getline( file, id ) )
		{
			while( !id.empty() && ( id.back() == '\n' || id.back() == '\r' || id.back() == ' ' ) )
				id.pop_back();
			if( !id.empty() )
				return id;
		}
	}
	return {};
}

std::filesystem::path Directory()
{
	std::filesystem::path base;
	if( const char* config = std::getenv( "XDG_CONFIG_HOME" ); config && *config )
		base = config;
	else if( const char* home = std::getenv( "HOME" ); home && *home )
		base = std::filesystem::path( home ) / ".config";
	else
		return {};
	return base / "LeTissier" / "Datamosh";
}

}  // namespace

std::unique_ptr< Transport > MakePlatformTransport()
{
	return std::make_unique< NoTransport >();
}

Environment PlatformEnvironment()
{
	Environment environment;
	environment.directory   = Directory();
	environment.fingerprint = ReadMachineId();
	environment.transport   = MakePlatformTransport();
	char host[ 256 ]        = {};
	if( gethostname( host, sizeof( host ) - 1 ) == 0 )
		environment.machineLabel = host;
	return environment;
}

void PinThisModule()
{
	Dl_info info{};
	if( dladdr( reinterpret_cast< void* >( &PinThisModule ), &info ) != 0 && info.dli_fname )
		dlopen( info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE );
}

}  // namespace datamosh::licence
