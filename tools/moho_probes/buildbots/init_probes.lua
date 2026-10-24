-- An init file (as bin/SupComDataPath.lua) that mounts the probes' hooks
-- ahead of the game, for the engine (`opensupcom --init`) and for the
-- original game (`SupremeCommander.exe /init`) alike. See README.md.

-- The game's folder (the one holding bin/ and gamedata/): as Linux sees it
-- for the engine, and as Wine sees it for the original game. Edit both for
-- another machine.
local FA_ROOT_LINUX = '/mnt/extrastorage/SteamLibrary/steamapps/common/Supreme Commander Forged Alliance'
local FA_ROOT_WINE = 'Z:\\mnt\\extrastorage\\SteamLibrary\\steamapps\\common\\Supreme Commander Forged Alliance'

local wine = string.find(InitFileDir, '\\', 1, true) ~= nil
local sep = wine and '\\' or '/'
local FA_ROOT = wine and FA_ROOT_WINE or FA_ROOT_LINUX
path = {}
table.insert(path, { dir = InitFileDir .. sep .. 'hooks', mountpoint = '/' })
table.insert(path, { dir = FA_ROOT .. sep .. 'gamedata' .. sep .. '*.scd', mountpoint = '/' })
table.insert(path, { dir = FA_ROOT, mountpoint = '/' })

hook = {
    '/schook'
}

protocols = {
    'http',
    'https',
}
