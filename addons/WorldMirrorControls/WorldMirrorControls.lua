-- World Mirror Controls temporarily changes only the current session. It captures
-- A/D and WoW's native mouseInvertYaw preference, then mirrors them whenever the
-- native world-mirror extension declares WorldMirrorRequested.
local controls = {}
local captured = false
local active = false
local original = {}

local function bindingFor(key)
    local action = GetBindingAction(key)
    return action ~= "" and action or nil
end

function controls:Capture()
    if captured then return end
    original.A = bindingFor("A")
    original.D = bindingFor("D")
    original.mouseInvertYaw = GetCVar("mouseInvertYaw")
    captured = true
end

local function mirroredYawPreference()
    return GetCVarBool(original.mouseInvertYaw) and "0" or "1"
end

function controls:SetEnabled(enabled)
    self:Capture()
    if enabled then
        SetBinding("A", original.D)
        SetBinding("D", original.A)
        SetCVar("mouseInvertYaw", mirroredYawPreference())
    else
        SetBinding("A", original.A)
        SetBinding("D", original.D)
        if original.mouseInvertYaw ~= nil then
            SetCVar("mouseInvertYaw", original.mouseInvertYaw)
        end
    end
    active = enabled and true or false
end

function controls:ApplyRequested()
    self:SetEnabled(WorldMirrorRequested == true)
end

function controls:IsActive()
    return active
end

_G.WorldMirrorControls = controls

local frame = CreateFrame("Frame")
frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("PLAYER_LOGOUT")
frame:SetScript("OnEvent", function(_, event)
    if event == "PLAYER_LOGIN" then
        controls:Capture()
        controls:ApplyRequested()
    elseif event == "PLAYER_LOGOUT" and active then
        controls:SetEnabled(false)
    end
end)
