#pragma once

// A minimal test registry. Deliberately not gtest: the whole point of these
// tests is that they run everywhere the plugin builds, and adding a package
// manager dependency to a graphics plugin is a good way to make CI the thing
// that breaks instead of the code.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace datamosh::test {

struct TestCase
{
	std::string           name;
	std::function< void() > body;
};

class Registry
{
public:
	static Registry& Instance()
	{
		static Registry registry;
		return registry;
	}

	void Add( std::string name, std::function< void() > body )
	{
		cases.push_back( { std::move( name ), std::move( body ) } );
	}

	const std::vector< TestCase >& Cases() const { return cases; }

	void Fail( const std::string& message )
	{
		failures.push_back( message );
	}

	std::vector< std::string > TakeFailures()
	{
		std::vector< std::string > taken;
		taken.swap( failures );
		return taken;
	}

private:
	std::vector< TestCase >    cases;
	std::vector< std::string > failures;
};

struct Registrar
{
	Registrar( const char* name, std::function< void() > body )
	{
		Registry::Instance().Add( name, std::move( body ) );
	}
};

}  // namespace datamosh::test

#define TEST( NAME )                                                     \
	static void NAME();                                                  \
	static ::datamosh::test::Registrar registrar_##NAME( #NAME, NAME );  \
	static void NAME()

#define CHECK( CONDITION )                                                             \
	do                                                                                 \
	{                                                                                  \
		if( !( CONDITION ) )                                                           \
		{                                                                              \
			::datamosh::test::Registry::Instance().Fail(                               \
				std::string( "  " ) + __FILE__ + ":" + std::to_string( __LINE__ ) +    \
				"  expected: " #CONDITION );                                           \
		}                                                                              \
	} while( false )

#define CHECK_NEAR( ACTUAL, EXPECTED, TOLERANCE )                                        \
	do                                                                                   \
	{                                                                                    \
		const double actualValue   = static_cast< double >( ACTUAL );                    \
		const double expectedValue = static_cast< double >( EXPECTED );                  \
		if( !( std::fabs( actualValue - expectedValue ) <= ( TOLERANCE ) ) )             \
		{                                                                                \
			char buffer[ 256 ];                                                          \
			std::snprintf( buffer, sizeof( buffer ),                                     \
			               "  %s:%d  %s == %.5f, expected %.5f (tolerance %.5f)",        \
			               __FILE__, __LINE__, #ACTUAL, actualValue, expectedValue,      \
			               static_cast< double >( TOLERANCE ) );                         \
			::datamosh::test::Registry::Instance().Fail( buffer );                       \
		}                                                                                \
	} while( false )
