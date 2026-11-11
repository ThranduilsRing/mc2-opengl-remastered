"""Deterministic registration of the stock Carver V mission-select views.

Pixel coordinates are stock 256px coordinates, with +Y down. Camera vectors
use +Y up and +Z toward the viewer. Poses map camera vectors into world space.
The orthographic model is the zero-distance limit of the perspective model.
"""
import argparse
import json
from itertools import combinations
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
from scipy.optimize import least_squares
from scipy.ndimage import distance_transform_edt, gaussian_filter
from scipy.spatial.transform import Rotation

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "art/source/campaign-planet"
NAMES = [f"mcl_cm_op{o}_{v}" for o, count in ((1, 6), (2, 3), (3, 4))
         for v in range(1, count + 1)]
UNIQUE = NAMES[:11]
ALIASES = {"mcl_cm_op3_3": "mcl_cm_op1_5", "mcl_cm_op3_4": "mcl_cm_op1_5"}


def lift(xy, geometry):
    """Back-project screen points onto a unit sphere (distance ratio R/d)."""
    cx, cy, radius, ratio = geometry
    p = (np.asarray(xy) - [cx, cy]) / radius
    p = p * [1, -1]
    rr = (p * p).sum(axis=-1)
    z = (ratio * rr + (1 - ratio**2) * np.sqrt(np.maximum(0, 1 - rr))) / (1 - ratio**2 + ratio**2 * rr)
    return np.concatenate((p * ((1 - ratio * z) / np.sqrt(1 - ratio**2))[..., None],
                           z[..., None]), axis=-1)


def project(v, geometry):
    cx, cy, radius, ratio = geometry
    f = radius * np.sqrt(1 - ratio**2) / (1 - ratio * v[..., 2])
    return np.stack((cx + f * v[..., 0], cy - f * v[..., 1]), axis=-1)


def features(image, name):
    rgb = np.asarray(image.convert("RGB"))
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    gray = cv2.createCLAHE(clipLimit=3, tileGridSize=(8, 8)).apply(gray)
    yy, xx = np.mgrid[:256, :256]
    mask = ((xx - 128)**2 + (yy - 116)**2 < 96**2)
    mask &= screen_mask(rgb, name, np.stack((xx, yy), axis=-1))
    kp, des = cv2.SIFT_create(contrastThreshold=.01).detectAndCompute(
        cv2.resize(gray, None, fx=3, fy=3),
        cv2.resize(np.uint8(mask) * 255, None, fx=3, fy=3, interpolation=cv2.INTER_NEAREST))
    return np.array([k.pt for k in kp]) / 3, des


def screen_mask(rgb, name, xy):
    # Authored reticles are 11px crosses on the screen-fixed 30px lattice.
    # Colour-keying them also rejects blue oceans and leaves dark cross ghosts.
    dx, dy = np.abs((xy[..., 0] - 8 + 15) % 30 - 15), np.abs((xy[..., 1] - 8 + 15) % 30 - 15)
    good = ~(((dx < 7) & (dy < 2)) | ((dy < 7) & (dx < 2)))
    if name == 'mcl_cm_op2_2':
        good &= ((xy[..., 0] - 65)**2 + (xy[..., 1] - 174)**2 > 54**2)
    return good


def register(source, out):
    cv2.setNumThreads(1)
    cv2.setRNGSeed(4)
    rng = np.random.default_rng(4)
    data = [features(Image.open(source / (name + '.png')), name) for name in UNIQUE]
    geometry = np.array([128., 116., 104., 0.])
    edges = []
    for i, j in combinations(range(len(UNIQUE)), 2):
        pi, di = data[i]
        pj, dj = data[j]
        forward = cv2.BFMatcher().knnMatch(di, dj, k=2)
        backward = {m.queryIdx: m.trainIdx for m, n in cv2.BFMatcher().knnMatch(dj, di, k=2)
                    if m.distance < .8 * n.distance}
        good = [m for m, n in forward if m.distance < .8 * n.distance
                and backward.get(m.trainIdx) == m.queryIdx]
        if len(good) < 8:
            continue
        a = pi[[m.queryIdx for m in good]]
        b = pj[[m.trainIdx for m in good]]
        av, bv = lift(a, geometry), lift(b, geometry)
        best = np.zeros(len(a), bool)
        samples = np.array([rng.choice(len(a), 3, replace=False) for _ in range(600)])
        aa, bb = av[samples], bv[samples]
        u, _, vt = np.linalg.svd(np.swapaxes(bb, 1, 2) @ aa)
        correction = np.tile(np.eye(3), (len(samples), 1, 1))
        correction[:, 2, 2] = np.linalg.det(u @ vt)
        matrices = u @ correction @ vt
        ok = np.linalg.norm(np.einsum('kij,nj->kni', matrices, av) - bv[None], axis=2) < .035
        spread = np.linalg.norm(np.cross(aa[:, 1] - aa[:, 0], aa[:, 2] - aa[:, 0]), axis=1)
        counts = ok.sum(axis=1) * (spread > .01)
        best = ok[np.argmax(counts)]
        if best.sum() < 8 or np.ptp(a[best], axis=0).min() < 20:
            continue
        rot = Rotation.align_vectors(bv[best], av[best])[0]
        if abs(rot.as_euler('yxz', degrees=True)[2]) > 70:
            continue
        edges.append((i, j, rot, a[best], b[best]))
        print('pair', UNIQUE[i], UNIQUE[j], int(best.sum()),
              np.round(rot.as_euler('yxz', degrees=True), 1).tolist(), flush=True)
    known = {0: Rotation.identity()}
    while len(known) < len(UNIQUE):
        choices = [e for e in edges if (e[0] in known) != (e[1] in known)]
        if not choices:
            missing = [UNIQUE[i] for i in range(len(UNIQUE)) if i not in known]
            raise ValueError(f'Unregistered views: {missing}')
        i, j, rot, a, b = max(choices, key=lambda e: len(e[3]))
        if i in known:
            known[j] = known[i] * rot.inv()
        else:
            known[i] = known[j] * rot
    x0 = np.r_[np.concatenate([known[i].as_rotvec() for i in range(1, len(UNIQUE))]), geometry]

    def unpack(x):
        return [Rotation.identity(), *Rotation.from_rotvec(x[:-4].reshape(-1, 3))], x[-4:]

    def residual(x, selected):
        poses, geom = unpack(x)
        return np.concatenate([((poses[i].apply(lift(a, geom)) -
                                 poses[j].apply(lift(b, geom))) * geom[2]).ravel()
                               for i, j, _, a, b in selected])

    lower = np.r_[np.full(len(x0) - 4, -np.inf), [125, 113, 101, 0]]
    upper = np.r_[np.full(len(x0) - 4, np.inf), [131, 119, 107, .7]]
    fit = least_squares(residual, x0, args=(edges,), bounds=(lower, upper),
                        loss='soft_l1', f_scale=1, max_nfev=150)
    poses, geometry = unpack(fit.x)
    clean = []
    for i, j, rot, a, b in edges:
        error = np.linalg.norm(poses[i].apply(lift(a, geometry)) -
                               poses[j].apply(lift(b, geometry)), axis=1) * geometry[2]
        ok = error < 2.5
        if ok.sum() >= 8:
            clean.append((i, j, rot, a[ok], b[ok]))
        else:
            print('rejected edge', UNIQUE[i], UNIQUE[j], 'mean', error.mean(), flush=True)
    fit = least_squares(residual, fit.x, args=(clean,), bounds=(lower, upper),
                        loss='soft_l1', f_scale=.7, max_nfev=150)
    poses, geometry = unpack(fit.x)
    result = {'geometry': geometry.tolist(),
              'poses': {name: poses[i].as_matrix().tolist() for i, name in enumerate(UNIQUE)},
              'aliases': ALIASES, 'landmarks': []}
    for i, j, _, a, b in clean:
        relative = poses[j].inv() * poses[i]
        error = np.linalg.norm(project(relative.apply(lift(a, geometry)), geometry) - b, axis=1)
        print('closed', UNIQUE[i], UNIQUE[j], len(a), 'mean_px', round(error.mean(), 3), flush=True)
        result['landmarks'].append({'a': UNIQUE[i], 'b': UNIQUE[j], 'xy_a': a.tolist(),
                                    'xy_b': b.tolist(), 'mean_px': float(error.mean())})
    print('geometry', geometry, 'cost', fit.cost, flush=True)
    out.write_text(json.dumps(result, indent=2) + '\n')


def remap(image, xy):
    return cv2.remap(np.asarray(image, np.float32), xy[..., 0].astype('float32'),
                     xy[..., 1].astype('float32'), cv2.INTER_LINEAR)


def sphere_grid(width, height):
    lat, lon = np.meshgrid((.5 - (np.arange(height) + .5) / height) * np.pi,
                          ((np.arange(width) + .5) / width - .5) * 2 * np.pi, indexing='ij')
    return np.stack((np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)), axis=-1)


def reconstruct(source, registration, out):
    """De-light and fuse registered views. Clouds remain a composite design.

    There are no cloud-free observations beneath the opaque stock clouds;
    inventing a separate surface there would not be a measured reconstruction.
    """
    cv2.setNumThreads(1)
    geometry = registration['geometry']
    world = sphere_grid(1024, 512)
    total = np.zeros_like(world)
    weights = np.zeros(world.shape[:2])
    lights = {}
    yy, xx = np.mgrid[:256, :256]
    xy = np.stack((xx, yy), axis=-1)
    camera_grid = lift(xy, geometry)
    disc = ((xx - geometry[0])**2 + (yy - geometry[1])**2) < (geometry[2] * .99)**2
    for name, matrix in registration['poses'].items():
        rgb = np.asarray(Image.open(source / (name + '.png')).convert('RGB'), np.float32) / 255
        reticles = ~screen_mask(rgb, 'no-moon', xy)
        rgb = cv2.inpaint(np.uint8(rgb * 255),
                          np.uint8(reticles) * 255, 3, cv2.INPAINT_TELEA).astype('float32') / 255
        value = rgb.max(axis=-1)
        saturation = (value - rgb.min(axis=-1)) / np.maximum(value, .001)
        valid = disc.copy()
        if name == 'mcl_cm_op2_2':
            valid &= (xx - 65)**2 + (yy - 174)**2 > 54**2
        white = valid & (saturation < .16) & (value > .14)
        normal = camera_grid[white]
        observations = value[white]

        def residual(light):
            return (light[0] + np.maximum(0, normal @ light[1:])) - observations

        fit = least_squares(residual, [.12, 0, .15, .55], loss='soft_l1', f_scale=.07,
                            bounds=([.01, -2, -2, -2], [.5, 2, 2, 2]))
        lights[name] = fit.x.tolist()
        camera = world @ np.array(matrix)
        lookup = project(camera, geometry)
        sample = remap(rgb, lookup)
        good = remap(valid.astype('float32'), lookup) > .99
        shade = fit.x[0] + np.maximum(0, camera @ fit.x[1:])
        weight = good * np.maximum(camera[..., 2] - .08, 0)**4 * shade**3
        total += np.clip(sample / np.maximum(shade[..., None], .06) * .72, 0, 1) * weight[..., None]
        weights += weight
        print('illumination', name, np.round(fit.x, 3).tolist(), flush=True)
    observed = weights > 1e-10
    albedo = total / np.maximum(weights[..., None], 1e-15)
    # The unobserved polar caps are explicitly recorded, not claimed as stock detail.
    nearest = distance_transform_edt(~observed, return_distances=False, return_indices=True)
    albedo[~observed] = albedo[tuple(nearest[:, ~observed])]
    # Smooth only unobserved texels; no measured coastline is displaced.
    smooth = gaussian_filter(albedo, (12, 12, 0), mode=('nearest', 'wrap', 'nearest'))
    albedo[~observed] = smooth[~observed]
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.uint8(np.clip(albedo * 255, 0, 255))).save(out / 'albedo.png')
    Image.fromarray(np.uint8(observed * 255)).save(out / 'coverage.png')
    (out / 'lighting.json').write_text(json.dumps(lights, indent=2) + '\n')
    print('observed sphere coverage', round(observed.mean() * 100, 2), flush=True)


def sample_sphere(albedo, world):
    h, w = albedo.shape[:2]
    lon = np.arctan2(world[..., 0], world[..., 2])
    lat = np.arcsin(np.clip(world[..., 1], -1, 1))
    xy = np.stack(((lon / (2 * np.pi) + .5) * w - .5,
                   (.5 - lat / np.pi) * h - .5), axis=-1)
    # Horizontal padding makes the longitude seam continuous without wrapping poles.
    padded = np.pad(albedo, ((0, 0), (1, 1), (0, 0)), mode='wrap')
    xy[..., 0] = np.mod(xy[..., 0], w) + 1
    xy[..., 1] = np.clip(xy[..., 1], 0, h - 1)
    return remap(padded, xy)


def material_detail(world):
    """Seamless world-space fBm; added detail never warps design coordinates."""
    detail = np.zeros(world.shape[:-1])
    for octave in range(4):
        p = world * (24 * 2**octave) + [11.7, 5.3, -8.1]
        cell = np.floor(p)
        f = p - cell
        f = f * f * (3 - 2 * f)
        value = np.zeros_like(detail)
        for dx, dy, dz in np.ndindex(2, 2, 2):
            vertex = cell + [dx, dy, dz]
            h = np.sin(vertex @ np.array([127.1, 311.7, 74.7])) * 43758.5453
            h -= np.floor(h)
            weight = np.prod(np.where(np.array([dx, dy, dz]), f, 1 - f), axis=-1)
            value += h * weight
        detail += (value - .5) / 2**octave
    return detail


def render(source, registration, maps, out, extension='png'):
    cv2.setNumThreads(1)
    albedo = np.asarray(Image.open(maps / 'albedo.png'), np.float32) / 255
    # Recover edge contrast at the measured position; generate new material
    # detail on the sphere rather than scaling the individual screen rasters.
    albedo = np.clip(albedo + .45 * (albedo - gaussian_filter(albedo, (.8, .8, 0), mode='wrap')), 0, 1)
    detail = material_detail(sphere_grid(albedo.shape[1], albedo.shape[0]))
    green = np.clip((albedo[..., 1] - albedo[..., 2]) * 6, 0, 1)
    cloud = np.clip((albedo.min(axis=-1) - .28) * 2, 0, 1)
    albedo = np.clip(albedo + detail[..., None] *
                     (green[..., None] * np.array([.08, .12, .055]) + cloud[..., None] * .055), 0, 1)
    lights = json.loads((maps / 'lighting.json').read_text())
    out.mkdir(parents=True, exist_ok=True)
    yy, xx = np.mgrid[:512, :512]
    xy = (np.stack((xx, yy), axis=-1) + .5) / 2 - .5
    geometry = registration['geometry']
    camera = lift(xy, geometry)
    distance = np.linalg.norm(xy - geometry[:2], axis=-1)
    for name in NAMES:
        pose_name = ALIASES.get(name, name)
        pose = np.array(registration['poses'][pose_name])
        light = np.array(lights[pose_name])
        world = camera @ pose.T
        surface = sample_sphere(albedo, world)
        shade = light[0] + np.maximum(0, camera @ light[1:])
        globe = np.clip(surface * shade[..., None] / .72, 0, 1)
        stock = np.asarray(Image.open(source / (name + '.png')).convert('RGB'), np.float32) / 255
        background = remap(stock, xy)
        # Retain the stock atmosphere, screen grid, star field, name bar and moon.
        # These are screen-space layers, excluded from spherical reconstruction.
        mask = np.clip((geometry[2] - distance - .4) / 1.2, 0, 1)
        if name == 'mcl_cm_op2_2':
            mask *= np.clip((np.linalg.norm(xy - [65, 174], axis=-1) - 52.5) / 1.5, 0, 1)
        # Rebuild the screen-space crosses geometrically, rather than copying
        # stock lighting into a cross-shaped cutout in the new sphere.
        dx = np.abs((xy[..., 0] - 8 + 15) % 30 - 15)
        dy = np.abs((xy[..., 1] - 8 + 15) % 30 - 15)
        cross = np.maximum(np.minimum(np.clip(5.5 - dx, 0, 1), np.clip(.75 - dy, 0, 1)),
                           np.minimum(np.clip(5.5 - dy, 0, 1), np.clip(.75 - dx, 0, 1))) * .35
        globe = globe * (1 - cross[..., None]) + np.array([0, .19, .32]) * cross[..., None]
        composed = globe * mask[..., None] + background * (1 - mask[..., None])
        image = Image.fromarray(np.uint8(np.clip(composed * 255, 0, 255)))
        if extension == 'tga':
            image.save(out / (name + '.tga'), compression='tga_rle')
        else:
            image.save(out / (name + '.png'))


def landmark_errors(source, rendered):
    """Independent stock/render SIFT displacement, in stock (256px) pixels."""
    cv2.setNumThreads(1)
    report = {}
    for name in NAMES:
        stock = Image.open(source / (name + '.png'))
        path = rendered / (name + '.tga')
        if not path.exists():
            path = rendered / (name + '.png')
        image = Image.open(path).resize((256, 256), Image.Resampling.LANCZOS)
        a, da = features(stock, name)
        b, db = features(image, name)
        matches = [m for m, n in cv2.BFMatcher().knnMatch(da, db, k=2) if m.distance < .7 * n.distance]
        displacement = np.array([b[m.trainIdx] - a[m.queryIdx] for m in matches])
        distance = np.linalg.norm(displacement, axis=-1)
        # Reject descriptor aliases before measuring. Never fit an image warp.
        accepted = distance[distance < 4]
        report[name] = {'landmarks': len(accepted), 'rejected': int((distance >= 4).sum()),
                        'mean_stock_px': float(accepted.mean()) if len(accepted) else None,
                        'p95_stock_px': float(np.percentile(accepted, 95)) if len(accepted) else None}
        print(name, report[name], flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['register', 'reconstruct', 'render', 'landmarks'], default='register', nargs='?')
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--out', type=Path, default=SOURCE / 'registration.json')
    parser.add_argument('--maps', type=Path, default=SOURCE / 'maps')
    args = parser.parse_args()
    if args.action == 'register':
        register(args.source, args.out)
    else:
        registration = json.loads((args.source / 'registration.json').read_text())
        if args.action == 'reconstruct':
            reconstruct(args.source, registration, args.maps)
        elif args.action == 'render':
            render(args.source, registration, args.maps, args.out)
        else:
            report = landmark_errors(args.source, args.out)
            (args.out / 'landmark-errors.json').write_text(json.dumps(report, indent=2) + '\n')
