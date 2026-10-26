-- Engine hook (data/fafengine, mounted by the engine at /fafengine and registered as a hook directory): appended to /lua/ui/game/gamemain.lua.
--
-- Runs an optional test script in the user interface after CreateUI: if /fafengine/lua/test/dynlights_camera.lua exists it is executed (it
-- zooms the camera out for the light tests). The file is not part of the engine, so on a normal install nothing happens and the check is one
-- file lookup.

do
    local oldCreateUI = CreateUI

    function CreateUI(isReplay)
        oldCreateUI(isReplay)

        local script = '/fafengine/lua/test/dynlights_camera.lua'
        if DiskGetFileInfo(script) then
            doscript(script)
        end
    end
end
