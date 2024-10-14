$gtk.disable_nil_punning!

SCREEN_SIZE = 48
WORLD_DIM = 1024

$gtk.dlopen("gc-arena-debug")

LEVEL_OBJECTS = WORLD_DIM * WORLD_DIM * 11
LEVEL_DATA = WORLD_DIM * WORLD_DIM * 500
LEVEL_ARENA = GC::Arena.allocate(objects: LEVEL_OBJECTS, storage: LEVEL_DATA)
LEVEL_CHUNKS = LEVEL_ARENA.eval do
  WORLD_DIM.times.map do |x|
    WORLD_DIM.times.map do |y|
      xpos = x * SCREEN_SIZE
      ypos = y * SCREEN_SIZE
      [
        { x: xpos, y: ypos, w: SCREEN_SIZE, h: SCREEN_SIZE, r: (x * 18) % 256, g: (y * 13) % 256, b: (x * y * 5) % 256 }.solid!,
        { x: xpos + 4, y: ypos + 4, text: "(#{x}, #{y})", r: 255, g: 255, b: 255, size_px: 10, vertical_alignment_enum: 0 }.label!,
      ]
    end
  end
end

WORLD_SIZE = WORLD_DIM * SCREEN_SIZE
X_SCREEN_CHUNKS = $grid.w.idiv(SCREEN_SIZE)
Y_SCREEN_CHUNKS = $grid.h.idiv(SCREEN_SIZE)
def tick(...)
  frame_time = (Time.now - $start).mult(1000)
  $start = Time.now

  $outputs.background_color = [0, 0, 0]

  $state.pos ||= { x: WORLD_SIZE.half, y: WORLD_SIZE.half }
  speed = $inputs.keyboard.key_held.shift ? 100 : 10
  pos = $state.pos = {
    x: $state.pos.x.add($inputs.left_right_perc.mult(speed)).clamp(0, WORLD_SIZE.subtract($grid.w)).to_i,
    y: $state.pos.y.add($inputs.up_down_perc.mult(speed)).clamp(0, WORLD_SIZE.subtract($grid.h)).to_i,
  }

  chunk = {
    x: pos.x.div(SCREEN_SIZE).clamp(2, (WORLD_DIM - 2).subtract(X_SCREEN_CHUNKS)).subtract(2),
    y: pos.y.div(SCREEN_SIZE).clamp(2, (WORLD_DIM - 2).subtract(Y_SCREEN_CHUNKS)).subtract(2),
  }

  onscreen = X_SCREEN_CHUNKS.add(4).times.flat_map do |x|
    Y_SCREEN_CHUNKS.add(4).times.map do |y|
      LEVEL_CHUNKS[chunk.x + x][chunk.y + y]
    end
  end

  $outputs.primitives << onscreen.flatten.map do |chunk|
    chunk.shift_rect(-pos.x, -pos.y)
  end

  render_time = (Time.now - $start).mult(1000)
  $max_frame_time = frame_time if frame_time && frame_time > $max_frame_time
  $avg_frame_time *= Kernel.tick_count
  $avg_frame_time /= Kernel.tick_count.add(1) unless $avg_frame_time.zero?
  $avg_frame_time += frame_time / Kernel.tick_count.add(1)
  $max_render_time = render_time if render_time > $max_render_time
  $avg_render_time *= Kernel.tick_count
  $avg_render_time /= Kernel.tick_count.add(1) unless $avg_render_time.zero?
  $avg_render_time += render_time / Kernel.tick_count.add(1)

  $outputs.debug << "FRAME TIME:"
  $outputs.debug << "  NOW: #{frame_time.to_sf}ms"
  $outputs.debug << "  MAX: #{$max_frame_time.to_sf}ms"
  $outputs.debug << "  AVG: #{$avg_frame_time.to_sf}ms"
  $outputs.debug << "RENDER TIME:"
  $outputs.debug << "  NOW: #{render_time.to_sf}ms"
  $outputs.debug << "  MAX: #{$max_render_time.to_sf}ms"
  $outputs.debug << "  AVG: #{$avg_render_time.to_sf}ms"
  $outputs.debug << "FPS: #{$gtk.current_framerate.to_sf}"
end

def reset(...)
  $start = Time.now
  $max_frame_time = 0
  $avg_frame_time = 0
  $max_render_time = 0
  $avg_render_time = 0
end

reset
