set(BINARY euclid-sftp)
add_executable(${BINARY} src/main.cpp src/SftpServer.cpp src/SftpSession.cpp)
set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${BINARY} PRIVATE include)
# libssh guards its server-side API behind WITH_SERVER, which the installed headers never define
# themselves - the symbols are in the library, but sftp_server_new()/sftp_server_free() stay
# invisible without this.
target_compile_definitions(${BINARY} PRIVATE WITH_SERVER)
target_link_libraries(${BINARY} PRIVATE euclidcore eucliddb euclidtransfer ssh Boost::thread Boost::program_options)

# euclid-sftp is the only binary that links both libssh and libmagic, and on Windows that is one
# binary too many: neither library can rely on a CRT strndup(), because MSVC has none, so both
# vendor their own - and the linker sees ssh.lib(misc.c.obj) and magic.lib(softmagic.obj) defining
# the same symbol:
#
#   magic.lib(softmagic.obj) : error LNK2005: strndup already defined in ssh.lib(misc.c.obj)
#   euclid-sftp.exe : fatal error LNK1169: one or more multiply defined symbols found
#
# Neither library can be dropped. libssh is what this program is; libmagic arrives through
# eucliddb, whose EQS and ENS repositories sniff a message's content type as they store it
# (MongoEqsRepository.cpp, MongoEnsRepository.cpp), so every binary touching the database pulls it.
#
# /FORCE:MULTIPLE tells the linker to keep the first definition and carry on. That is safe for this
# symbol in substance - both implementations are the same handful of lines, allocate and copy at
# most n bytes - but it is a blunt instrument: it silences duplicate-symbol errors for this
# executable as a whole, not just for strndup. The accompanying LNK4006 warnings are deliberately
# NOT suppressed, because they are the only remaining signal if a duplicate that does matter ever
# turns up here.
#
# The clean fix belongs upstream, in a vcpkg overlay port that stops one of the two exporting
# strndup. This is the cheaper one, and it is scoped to the single target that has the problem.
if (MSVC)
    target_link_options(${BINARY} PRIVATE /FORCE:MULTIPLE)
endif ()
