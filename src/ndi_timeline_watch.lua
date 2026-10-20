-- Timeline clip watcher helper, Lua flavor (one-click Timeline (Auto);
-- supersedes the Python helper as the primary path on macOS and Windows).
--
-- Runs under Resolve's OWN bundled script interpreter (fuscript: macOS
-- Contents/Libraries/Fusion/fuscript, Windows fuscript.exe beside
-- Resolve.exe), so it needs no Python - or anything else - installed. The
-- Python helper (ndi_timeline_watch.py) remains the documented fallback for
-- hosts without fuscript. Why not Python: both OSes ship a python at the
-- canonical path that is not one (issue #34 - Apple's /usr/bin/python3 CLT
-- shim, the Windows Store alias stub), and on Windows Resolve's
-- fusionscript.dll only binds a PEP 514 registry-REGISTERED interpreter -
-- never the process that loaded it - so a bundled/embedded Python can never
-- work (LEARNINGS 2026-09-01, the fusionscript registry findings).
--
-- Protocol (read by src/TimelineClipWatcher.cpp on macOS and
-- src/WinTimelineWatch.cpp on Windows), identical to the Python helper: one
-- line per poll (~2/s); a line starting with '!' is a status message for the
-- plugin log; a line starting with '@color' carries the timeline's colour
-- settings (issue #57 Part 3, tab-separated, see src/TimelineColor.h), sent
-- when they change; any other line is the current clip's file path ('' =
-- none).
-- Printing every poll doubles as a liveness heartbeat. Exits when the reader
-- hangs up (stdout write fails => plugin/Resolve gone).

local POLL_SECONDS = 0.5
local COLOR_EVERY = 4        -- colour settings every 4th poll (~2 s)

-- Colour settings the timeline renders in (Timeline Color = Auto, issue #57).
-- Timeline:GetSetting follows a timeline's own custom colour settings and
-- falls back to the project's; the plugin's parser (src/TimelineColor.h)
-- knows which fields Resolve leaves stale in each mode.
local COLOR_KEYS = {"colorScienceMode", "separateColorSpaceAndGamma",
                    "colorSpaceTimeline", "colorSpaceTimelineGamma",
                    "colorSpaceOutput", "colorSpaceOutputGamma"}

local function color_line(project, timeline)
  local fields = {"@color"}
  for _, key in ipairs(COLOR_KEYS) do
    local v = nil
    if timeline then v = timeline:GetSetting(key) end
    if (v == nil or v == "") and project then v = project:GetSetting(key) end
    if type(v) ~= "string" then v = "" end
    fields[#fields + 1] = (v:gsub("[\t\r\n]", " "))
  end
  return table.concat(fields, "\t")
end

local function say(line)
  -- One protocol line per call: a multi-line error text would otherwise be
  -- read back as clip paths.
  line = tostring(line):gsub("[\r\n]+", " ")
  local ok = io.stdout:write(line, "\n")
  if not ok then os.exit(0) end   -- reader hung up: plugin is gone
  io.stdout:flush()
end

local function sleep(seconds)
  if bmd and bmd.wait then
    bmd.wait(seconds)
  else
    local deadline = os.clock() + seconds  -- crude, but only if bmd.wait vanishes
    while os.clock() < deadline do end
  end
end

if not (bmd and bmd.scriptapp) then
  say("!fuscript provided no bmd.scriptapp - cannot reach Resolve")
  os.exit(1)
end

local resolve = nil
local warned_no_app = false
local last_color = nil
local poll = 0
while true do
  local path = ""
  local color = nil
  local ok, err = pcall(function()
    if resolve == nil then
      resolve = bmd.scriptapp("Resolve")
      if resolve == nil and not warned_no_app then
        warned_no_app = true
        say("!scriptapp('Resolve') returned nothing - is external scripting " ..
            "enabled? (Preferences > System > General)")
      end
    end
    if resolve ~= nil then
      local pm = resolve:GetProjectManager()
      local project = pm and pm:GetCurrentProject() or nil
      local timeline = project and project:GetCurrentTimeline() or nil
      if project and poll % COLOR_EVERY == 0 then
        color = color_line(project, timeline)
      end
      local item = timeline and timeline:GetCurrentVideoItem() or nil
      local pool = item and item:GetMediaPoolItem() or nil
      if pool then
        path = pool:GetClipProperty("File Path")
        if type(path) ~= "string" then path = "" end
      end
    end
  end)
  if not ok then
    -- Stale object across a project/timeline switch - reconnect next poll.
    resolve = nil
    say("!query failed (will reconnect): " .. tostring(err))
    path = ""
  end
  if color and color ~= last_color then
    say(color)
    last_color = color
  end
  say(path)
  poll = poll + 1
  sleep(POLL_SECONDS)
end
