-- Visor Rally Nativo: exporta somente dados de navegacao por UDP local.
local M = {}
local udp
local elapsed = 0
local roadElapsed = 10
local roads = {}
local lastX, lastY
local port = 4445

local function round(v)
  return math.floor(v * 10 + 0.5) / 10
end

local function openSocket()
  if udp then return true end
  local ok, socket = pcall(require, 'socket')
  if not ok or not socket or not socket.udp then
    ok, socket = pcall(require, 'socket.socket')
  end
  if not ok or not socket or not socket.udp then
    log('E','visorRallyNav','LuaSocket UDP indisponivel')
    return false
  end
  udp = socket.udp()
  if not udp then return false end
  udp:settimeout(0)
  log('I','visorRallyNav','Navegacao nativa para 127.0.0.1:'..port)
  return true
end

local function updateRoads(pos)
  roads = {}
  local graph = map and map.getMap and map.getMap()
  if not graph or not graph.nodes then return end
  local candidates = {}
  local seen = {}
  for id, node in pairs(graph.nodes) do
    if node.pos and not node.hiddenInNavi then
      local dx,dy = node.pos.x-pos.x, node.pos.y-pos.y
      if dx*dx+dy*dy < 800*800 then
        for otherId, link in pairs(node.links or {}) do
          local other = graph.nodes[otherId]
          local aId,bId=tostring(id),tostring(otherId)
          local key=aId<bId and (aId..'|'..bId) or (bId..'|'..aId)
          if not seen[key] and other and other.pos and
             not other.hiddenInNavi and not link.hiddenInNavi then
            seen[key]=true
            local bx,by = other.pos.x-pos.x,other.pos.y-pos.y
            candidates[#candidates+1] = {
              d=math.min(dx*dx+dy*dy,bx*bx+by*by),
              a=node.pos.x,b=node.pos.y,c=other.pos.x,dest=other.pos.y
            }
          end
        end
      end
    end
  end
  table.sort(candidates,function(a,b) return a.d<b.d end)
  for i=1,math.min(200,#candidates) do
    local p=candidates[i]
    roads[#roads+1]={p.a,p.b,p.c,p.dest}
  end
  lastX,lastY=pos.x,pos.y
end

local function buildPacket()
  local vehicle = be and be:getPlayerVehicle(0)
  if not vehicle then return {protocol='RLYNAV1',status='no_vehicle'} end
  local pos=vehicle:getPosition()
  local dir=vehicle:getDirectionVector()
  local norm=math.sqrt(dir.x*dir.x+dir.y*dir.y)
  if norm<0.001 then return {protocol='RLYNAV1',status='no_vehicle'} end
  local moved = not lastX or (pos.x-lastX)^2+(pos.y-lastY)^2>100*100
  if roadElapsed>=2 or moved then
    updateRoads(pos)
    roadElapsed=0
  end
  local nearby={}
  for _,p in ipairs(roads) do
    nearby[#nearby+1]={round(p[1]-pos.x),round(p[2]-pos.y),round(p[3]-pos.x),round(p[4]-pos.y)}
  end
  local route={}
  local remaining=0
  local gm=core_groundMarkers
  local path=gm and gm.routePlanner and gm.routePlanner.path
  if path then
    for _,point in ipairs(path) do
      if point.pos then
        local x,y=point.pos.x-pos.x,point.pos.y-pos.y
        if x*x+y*y<2000*2000 then route[#route+1]={round(x),round(y)} end
        if #route>=160 then break end
      end
    end
    if gm.getPathLength then
      local ok,value=pcall(gm.getPathLength)
      if ok and type(value)=='number' and value==value and value<math.huge and value>=0 then
        remaining=round(value)
      end
    end
  end
  return {protocol='RLYNAV1',status='ok',dir={dir.x/norm,dir.y/norm},
    roads=nearby,route=route,remaining=remaining}
end

local function onUpdate(dtReal)
  elapsed=elapsed+(dtReal or 0)
  roadElapsed=roadElapsed+(dtReal or 0)
  if elapsed<0.2 then return end
  elapsed=0
  if not openSocket() then return end
  local ok,packet=pcall(buildPacket)
  if not ok then
    log('E','visorRallyNav',tostring(packet))
    return
  end
  local payload=jsonEncode(packet)
  if #payload<50000 then udp:sendto(payload,'127.0.0.1',port) end
end

local function onExtensionLoaded()
  setExtensionUnloadMode(M,'manual')
  openSocket()
end
local function onExtensionUnloaded()
  if udp then udp:close(); udp=nil end
end
local function onClientEndMission()
  roads={}; lastX=nil; lastY=nil; roadElapsed=10
end

M.onUpdate=onUpdate
M.onExtensionLoaded=onExtensionLoaded
M.onExtensionUnloaded=onExtensionUnloaded
M.onClientEndMission=onClientEndMission
return M
