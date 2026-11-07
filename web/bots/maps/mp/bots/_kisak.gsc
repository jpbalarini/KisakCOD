// _kisak.gsc -- NOT part of upstream Bot Warfare. Starts it on this engine; see web/platform/bots_web.cpp and web/bots/README.md.
//
// Upstream expects the CoD4x server, which provides a few built-in functions; its own adapter (scripts/mp/bots_adapter_cod4x.gsc) maps them
// into level.bot_builtins. This is the same table, mapped onto the built-ins bots_web.cpp adds to this engine. The engine runs init() right
// after the gametype's startup callback (g_scr_main_mp.cpp, Scr_StartupGameType), which is where upstream's callbacksetup.gsx started it.
//
// Nothing here adds a bot: bots_manage_add / bots_manage_fill stay 0 (upstream's defaults) until the server console says otherwise
// (addbot, or set bots_manage_fill 8).
init()
{
	// A public server must not hand the bot menu / host powers to whoever connects first (upstream defaults to yes), and with no host to wait
	// for, bots_manage_add would stall 10 s at the start of every map.
	if ( getdvar( "bots_main_firstIsHost" ) == "" )
	{
		setdvar( "bots_main_firstIsHost", "0" );
	}

	if ( getdvar( "bots_main_waitForHostTime" ) == "" )
	{
		setdvar( "bots_main_waitForHostTime", "0" );
	}

	level.bot_builtins = [];
	level.bot_builtins[ "printconsole" ] = ::do_printconsole;
	level.bot_builtins[ "fileexists" ] = ::do_fileexists;
	level.bot_builtins[ "botaction" ] = ::do_botaction;
	level.bot_builtins[ "botstop" ] = ::do_botstop;
	level.bot_builtins[ "botmovement" ] = ::do_botmovement;
	level.bot_builtins[ "botmoveto" ] = ::do_botmoveto;
	level.bot_builtins[ "botmeleeparams" ] = ::do_botmeleeparams;
	level.bot_builtins[ "botangles" ] = ::do_botangles;
	level.bot_builtins[ "isbot" ] = ::do_isbot;
	level.bot_builtins[ "fs_fopen" ] = ::do_fs_fopen;
	level.bot_builtins[ "fs_fclose" ] = ::do_fs_fclose;
	level.bot_builtins[ "fs_readline" ] = ::do_fs_readline;
	level.bot_builtins[ "fs_writeline" ] = ::do_fs_writeline;

	level thread maps\mp\bots\_bot::init();
	level thread maps\mp\bots\_bot_chat::init();
}

do_printconsole( s )
{
	printconsole( s );
}

do_fileexists( file )
{
	return fs_testfile( "scriptdata/" + file );
}

do_botaction( action )
{
	self botaction( action );
}

do_botstop()
{
	self botstop();
}

do_botmovement( forward, right )
{
	self botmovement( forward, right );
}

do_botmoveto( where )
{
	// CoD4x walks to the point by itself; here botmovement() above already walks relative to the view.
}

do_botmeleeparams( yaw, dist )
{
	self botmeleeparams( yaw, dist );
}

do_botangles( angles )
{
	self setplayerangles( angles );
}

do_isbot()
{
	return self isbot();
}

do_fs_fopen( file, mode )
{
	return fs_fopen( "scriptdata/" + file, mode );
}

do_fs_fclose( fh )
{
	fs_fclose( fh );
}

do_fs_readline( fh )
{
	return fs_readline( fh );
}

do_fs_writeline( fh, contents )
{
	fs_writeline( fh, contents );
}
