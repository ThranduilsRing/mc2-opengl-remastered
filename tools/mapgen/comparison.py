"""Make a review sheet of real harness captures beside their pixel-art plans.

No launches or install writes. Captures retain the HUD and original perspective;
the plan is north-up. This is a content review, not a registered pixel diff.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw

from tools.mapgen.common import load_json, output_path
from tools.mapgen.raster import crop_base, render


def build(layout_path, captures, out):
    layout = load_json(layout_path)
    out = output_path(out)
    out.parent.mkdir(parents=True,exist_ok=True)
    sheet = Image.new('RGB',(1700,1350),(22,26,30))
    draw = ImageDraw.Draw(sheet)
    draw.text((20,12),f'{layout["name"]}: generated plan / in-game Vulkan backbuffer',fill='white')
    draw.text((20,32),'North-up plan, native Quonset scale enlarged with nearest-neighbour; game view is perspective with HUD.',fill='white')
    draw.text((20,50),'Roads and cement in the plan are reservations: this first writer emits flat stock grass terrain.',fill=(255,210,110))
    for i in range(2):
        y = 90+i*620
        plan_path = out.with_name(out.stem+f'_base{i}_plan.png')
        render(layout,plan_path,crop=crop_base(layout,i))
        with Image.open(plan_path) as plan:
            scale = max(1,min(560//plan.width,560//plan.height))
            enlarged = plan.convert('RGB').resize((plan.width*scale,plan.height*scale),Image.Resampling.NEAREST)
        sheet.paste(enlarged,(20+(560-enlarged.width)//2,y+20+(560-enlarged.height)//2))
        frame_path = Path(captures)/f'{layout["name"]}_base{i}_topdown.png'
        with Image.open(frame_path) as frame:
            frame = frame.convert('RGB').resize((1080,608),Image.Resampling.LANCZOS)
            sheet.paste(frame,(600,y))
        draw.text((20,y),f'Base {i}: pixel-art plan',fill='white')
        draw.text((600,y-17),f'Base {i}: {frame_path.name}',fill='white')
    sheet.save(out)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout',type=Path)
    parser.add_argument('--captures',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args = parser.parse_args()
    print(build(args.layout,args.captures,args.out))


if __name__ == '__main__':
    main()
