"""Synchronized VTK viewports with foreground body RGB axes."""
from math import cos, sin, tau

import numpy as np
from PIL import Image, ImageChops
import vtk
from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray, vtk_to_numpy
from .geometry import camera, fpv_angles, level_fpv_pose
from .composition import FPV_MODES, rounded_mask, view_overlays


# Lift EDL midtones while preserving the dark background and white overlays.
EDL_TONES = [v if v <= 14 else round(14 + 241 * ((v - 14) / 241) ** .66)
             for v in range(256)] * 3
# Fixed world-oriented projection for the FPV attitude indicator.
ATTITUDE_PROJECTION = np.array([[1., -1., 0.], [-1., -1., 2.], [0., 0., 0.]])
ATTITUDE_PROJECTION[:2] /= np.linalg.norm(ATTITUDE_PROJECTION[:2], axis=1)[:, None]


def point_mesh(xyz, rgb):
    poly=vtk.vtkPolyData()
    pts=vtk.vtkPoints(); pts.SetData(numpy_to_vtk(np.ascontiguousarray(xyz),deep=True)); poly.SetPoints(pts)
    cells=vtk.vtkCellArray()
    offsets = np.arange(len(xyz) + 1, dtype=np.int64)
    cells.SetData(numpy_to_vtkIdTypeArray(offsets, deep=True),
                  numpy_to_vtkIdTypeArray(offsets[:-1], deep=True))
    poly.SetVerts(cells)
    poly.GetPointData().SetScalars(numpy_to_vtk(np.ascontiguousarray(rgb,dtype='u1'),deep=True))
    return poly


def point_actor(mesh, size):
    mapper=vtk.vtkPolyDataMapper(); mapper.SetInputData(mesh)
    actor=vtk.vtkActor(); actor.SetMapper(mapper); actor.GetProperty().SetPointSize(size)
    actor.GetProperty().LightingOff()
    return actor


def line_actor(points, color, width=3):
    poly=vtk.vtkPolyData(); pts=vtk.vtkPoints()
    pts.SetData(numpy_to_vtk(np.ascontiguousarray(points,dtype=float),deep=True)); poly.SetPoints(pts)
    cells=vtk.vtkCellArray(); cells.InsertNextCell(len(points))
    for i in range(len(points)): cells.InsertCellPoint(i)
    poly.SetLines(cells)
    mapper=vtk.vtkPolyDataMapper(); mapper.SetInputData(poly)
    actor=vtk.vtkActor(); actor.SetMapper(mapper)
    actor.GetProperty().SetColor(*color); actor.GetProperty().SetLineWidth(width); actor.GetProperty().LightingOff()
    return actor


SPATIAL_COLORS = {
    "plan": (.20, .92, .50),
    "goal": (1., .796, .388),
    "corridor": (.30, .70, 1.),
    "camera": (.78, .65, .94),
    "scan": (118, 246, 255),
}


def spatial_polydata(groups, surface=False):
    points = np.concatenate(groups, axis=0)
    lengths = np.array([len(group) for group in groups], dtype=np.int64)
    offsets = np.r_[0, np.cumsum(lengths)]
    cells = vtk.vtkCellArray()
    cells.SetData(numpy_to_vtkIdTypeArray(offsets, deep=True),
                  numpy_to_vtkIdTypeArray(np.arange(len(points), dtype=np.int64), deep=True))
    vertices = vtk.vtkPoints()
    vertices.SetData(numpy_to_vtk(np.ascontiguousarray(points, dtype=float), deep=True))
    poly = vtk.vtkPolyData()
    poly.SetPoints(vertices)
    if surface:
        poly.SetPolys(cells)
    else:
        poly.SetLines(cells)
    return poly


def overlay_lines(groups, color, width, *, screen_space=False, colors=None, filled=False):
    coordinate = vtk.vtkCoordinate()
    if screen_space:
        coordinate.SetCoordinateSystemToDisplay()
    else:
        coordinate.SetCoordinateSystemToWorld()
    mapper = vtk.vtkPolyDataMapper2D()
    mesh = spatial_polydata(groups, surface=filled)
    if colors is not None:
        rgb = np.rint(np.asarray(colors) * 255).astype(np.uint8)
        mesh.GetCellData().SetScalars(numpy_to_vtk(rgb, deep=True))
        mapper.SetScalarModeToUseCellData()
        mapper.SetColorModeToDirectScalars()
    mapper.SetInputData(mesh)
    mapper.SetTransformCoordinate(coordinate)
    actor = vtk.vtkActor2D()
    actor.SetMapper(mapper)
    actor.GetProperty().SetColor(*color)
    actor.GetProperty().SetLineWidth(width)
    return actor


def clip_near_plane(points, depths, near):
    """Split a polyline at the camera plane before projecting a 2D overlay."""
    if len(points) < 2:
        return
    visible = depths >= near
    if visible.all():
        yield points
        return

    def intersection(a, b):
        fraction = (near - depths[a]) / (depths[b] - depths[a])
        return points[a] + fraction * (points[b] - points[a])

    bounds = np.flatnonzero(np.diff(np.r_[False, visible, False]))
    for start, stop in bounds.reshape(-1, 2):
        parts = [points[start:stop]]
        if start:
            parts.insert(0, intersection(start - 1, start)[None, :])
        if stop < len(points):
            parts.append(intersection(stop - 1, stop)[None, :])
        yield np.concatenate(parts)


def visible_world_lines(scene, groups, colors=None):
    camera = scene.GetActiveCamera()
    eye, forward = np.asarray(camera.GetPosition()), np.asarray(camera.GetDirectionOfProjection())
    near = camera.GetClippingRange()[0]
    visible, visible_colors = [], []
    for i, points in enumerate(groups):
        points = np.asarray(points, dtype=float)
        for part in clip_near_plane(points, (points - eye) @ forward, near):
            visible.append(part)
            if colors is not None:
                visible_colors.append(colors[i])
    return visible, visible_colors if colors is not None else None


def mission_actor(groups, color, width, *, surface=False):
    mapper = vtk.vtkPolyDataMapper()
    mapper.SetInputData(spatial_polydata(groups, surface))
    if not surface:
        mapper.SetResolveCoincidentTopologyToPolygonOffset()
    actor = vtk.vtkActor()
    actor.SetMapper(mapper)
    props = actor.GetProperty()
    props.SetColor(*color)
    props.LightingOff()
    if surface:
        props.SetOpacity(.14)
        props.BackfaceCullingOn()
    else:
        props.SetLineWidth(width)
    return actor


def view_camera(pose, mode, rear, oblique, elapsed, fpv_pose=None, *, zoom=1.):
    distance, height, side = oblique
    if mode in FPV_MODES:
        if mode == "fpv":
            pose = fpv_pose if fpv_pose is not None else level_fpv_pose(pose, fpv_angles(pose))
        forward, _, up = pose[:3, :3].T
        position = pose[:3, 3] + forward * .12
        return position, position + forward * 8., up
    if mode == "orbit":
        phase = elapsed * tau / 9.
        geometry = (cos(phase) * distance, height * .58, sin(phase) * distance)
    else:
        geometry = {
            "main": rear,
            "rear": rear,
            "oblique": oblique,
            "front": (-distance, height * .44, 0.),
            "left": (0., height * .32, distance),
            "right": (0., height * .32, -distance),
            "rear_left": (distance * .66, height * .59, distance * .65),
            "rear_right": (distance * .66, height * .59, -distance * .65),
            "high": (distance * .30, height * 2.1, side),
            "low": (distance * .63, height * .12, side),
        }[mode]
    position, target = camera(pose, *geometry, lookahead=1.5 if mode in {"main", "rear"} else 0.)
    return target + (position-target)/zoom, target, (0., 0., 1.)


def zoomed_fov(fov, zoom):
    return float(np.clip(np.rad2deg(2 * np.arctan(np.tan(np.deg2rad(fov/2)) / zoom)), 1., 170.))


def goal_disks(scene, goals, diameter):
    """Project each visible world-space goal and draw one solid screen-space disk."""
    angles = np.linspace(0, tau, 32, endpoint=False)
    disk = diameter/2 * np.column_stack((np.cos(angles), np.sin(angles), np.zeros_like(angles)))
    camera = scene.GetActiveCamera()
    eye, direction = np.asarray(camera.GetPosition()), np.asarray(camera.GetDirectionOfProjection())
    near, far = camera.GetClippingRange()
    width, height = scene.GetSize()
    radius = diameter/2
    disks = []
    for shape in goals:
        for group in shape.goals:
            for point in group:
                if not near <= np.dot(point-eye, direction) <= far:
                    continue
                scene.SetWorldPoint(*point, 1.)
                scene.WorldToDisplay()
                x, y, _ = scene.GetDisplayPoint()
                if -radius <= x <= width+radius and -radius <= y <= height+radius:
                    disks.append(disk + [x, y, 0.])
    return disks


class Renderer:
    def __init__(self,width=1280,height=720,edl=True,axis=.48,axis_pixels=28,point_size=2,
                 distance=3.8,camera_height=1.35,side=0.,fov=65,inset_distance=10,inset_height=6.7,
                 *, supersample=1, cloud_color=None, trajectory_color=(1,.73,.18), trajectory_width=5, plan_width=6, axis_width=5, goal_size=24, clean=False, slam_view=False, view_modes=None, main_mode="rear", view_widths=None, overlays=None, view_zooms=None):
        self.output_size = (width, height)
        self.scale = supersample
        self.edl = edl
        self.clean = clean
        self.slam_view = slam_view
        self.width, self.height = width * supersample, height * supersample
        self.axis, self.axis_pixels = axis, axis_pixels * supersample
        self.point_size = point_size * supersample
        self.cloud_color = cloud_color
        self.trajectory_color = trajectory_color
        self.trajectory_width = trajectory_width * supersample
        self.plan_width = plan_width * supersample
        self.axis_width = axis_width * supersample
        self.goal_size = goal_size * supersample
        self.fov = fov
        self.camera_args=(distance,camera_height,side)
        self.inset_args=(inset_distance,inset_height,0.)
        self.windows=[]
        self.views=[]
        self.cloud_actors=[]
        self.range_cloud = self.range_position = None
        self.dynamic=[]
        self.scan_actors = []
        self.scan = None
        self.view_modes = view_modes
        self.modes = (main_mode, *(("oblique",) if view_modes is None else view_modes))
        settings = (None,) * len(self.modes) if overlays is None else overlays
        self.visible_overlays = tuple(view_overlays(mode, selected) for mode, selected in zip(self.modes, settings))
        self.view_zooms = (1.,) * len(self.modes) if view_zooms is None else tuple(view_zooms)
        view_widths = (.32,) * (len(self.modes)-1) if view_widths is None else view_widths
        for index in range(len(self.modes)):
            window=vtk.vtkRenderWindow()
            factor = 1. if index == 0 else min(1., max(.32, view_widths[index-1]))
            window.SetSize(max(2, round(self.width * factor)), max(2, round(self.height * factor)))
            window.SetOffScreenRendering(1); window.SetMultiSamples(0); window.SetNumberOfLayers(2)
            window.SetAlphaBitPlanes(1)
            self.windows.append(window)
            scene=vtk.vtkRenderer(); scene.SetViewport(0,0,1,1); scene.SetLayer(0)
            scene.SetBackground(.018,.028,.047); scene.SetPreserveColorBuffer(False)
            scene.SetBackgroundAlpha(0)
            if slam_view:
                # VTK draws its gradient opaque even when the clear alpha is zero.
                scene.SetBackground(.058, .070, .092)
            if edl:
                basic=vtk.vtkRenderStepsPass(); effect=vtk.vtkEDLShading(); effect.SetDelegatePass(basic)
                scene.SetPass(effect)
            overlay=vtk.vtkRenderer(); overlay.SetViewport(0,0,1,1); overlay.SetLayer(1)
            overlay.SetActiveCamera(scene.GetActiveCamera()); overlay.SetPreserveDepthBuffer(False)
            overlay.SetBackgroundAlpha(0)
            scene.GetActiveCamera().SetViewAngle(fov)
            window.AddRenderer(scene); window.AddRenderer(overlay)
            self.views.append((scene,overlay))
        self.label=vtk.vtkTextActor(); self.label.SetPosition(22*self.scale,22*self.scale)
        self.label.GetTextProperty().SetFontSize(17*self.scale); self.label.GetTextProperty().SetColor(.9,.94,1)
        self.views[0][1].AddViewProp(self.label)

    def update_cloud(self,cloud):
        for scene,actor in self.cloud_actors: scene.RemoveActor(actor)
        self.cloud_actors=[]
        self.range_cloud = cloud if getattr(cloud, "range_color", False) and self.cloud_color is None else None
        self.range_position = None
        rgb = cloud.rgb if self.cloud_color is None else np.broadcast_to(self.cloud_color, cloud.rgb.shape)
        mesh = point_mesh(cloud.xyz, rgb)
        for scene, _ in self.views:
            # Reuse the CPU mesh while each OpenGL context retains its own mapper.
            actor = point_actor(mesh, self.point_size)
            scene.AddActor(actor)
            self.cloud_actors.append((scene, actor))

    def update_range_colors(self, position):
        if self.range_cloud is None or np.array_equal(position, self.range_position):
            return
        colors = numpy_to_vtk(self.range_cloud.colors_from(position), deep=True)
        self.cloud_actors[0][1].GetMapper().GetInput().GetPointData().SetScalars(colors)
        self.range_position = position.copy()

    def update_scan(self, scan):
        if scan is self.scan:
            return
        self.scan = scan
        for scene, actor in self.scan_actors:
            scene.RemoveActor(actor)
        self.scan_actors.clear()
        if scan is not None and len(scan):
            sample = scan[::max(1, (len(scan) + 79999) // 80000)]
            rgb = np.broadcast_to(SPATIAL_COLORS["scan"], sample.shape)
            mesh = point_mesh(sample, rgb)
            for (scene, _), visible in zip(self.views, self.visible_overlays):
                if "scan" not in visible:
                    continue
                actor = point_actor(mesh, self.point_size)
                # Preserve the map's distance colors where the current scan overlaps it.
                actor.GetProperty().SetOpacity(.25)
                actor.GetMapper().SetResolveCoincidentTopologyToPolygonOffset()
                actor.GetMapper().SetRelativeCoincidentTopologyPointOffsetParameter(-4)
                scene.AddActor(actor)
                self.scan_actors.append((scene, actor))

    def add_spatial(self, scene, overlay, frame, visible):
        def add(groups, color, width, surface=False):
            if groups:
                actor = mission_actor(groups, SPATIAL_COLORS[color], width*self.scale, surface=surface)
                scene.AddActor(actor)
                self.dynamic.append((scene, actor))

        def lines(shapes):
            return tuple(group for shape in shapes for group in shape.lines) + tuple(
                segment for shape in shapes for group in shape.segments for segment in group)

        if "plan" in visible:
            old = []
            for line in lines(frame.previous_plan):
                for first, last in zip(line[:-1], line[1:]):
                    count = max(1, min(120, int(np.linalg.norm(last-first)/.22)+1))
                    for i in range(count):
                        ends = np.array([i, i+.55])[:, None] / count
                        old.append(first + ends * (last-first))
            old, _ = visible_world_lines(scene, old)
            if old:
                actor = overlay_lines(old, (.72, .77, .84), max(1., self.plan_width * .55))
                actor.GetProperty().SetOpacity(.8)
                overlay.AddViewProp(actor)
                self.dynamic.append((overlay, actor))
            planned, colors = [], []
            for shape in frame.plan:
                groups = lines((shape,))
                planned.extend(groups)
                colors.extend([shape.color or SPATIAL_COLORS["plan"]] * len(groups))
            planned, colors = visible_world_lines(scene, planned, colors)
            if planned:
                actor = overlay_lines(planned, (.035, .055, .072), self.plan_width + 2 * self.scale)
                overlay.AddViewProp(actor)
                self.dynamic.append((overlay, actor))
                rgb = colors if any(color != SPATIAL_COLORS["plan"] for color in colors) else None
                actor = overlay_lines(planned, SPATIAL_COLORS["plan"], self.plan_width, colors=rgb)
                overlay.AddViewProp(actor)
                self.dynamic.append((overlay, actor))
        if "goal" in visible and frame.goal:
            disks = goal_disks(scene, frame.goal, self.goal_size)
            if disks:
                actor = overlay_lines(disks, SPATIAL_COLORS["goal"], 1., screen_space=True, filled=True)
                overlay.AddViewProp(actor)
                self.dynamic.append((overlay, actor))
        if "corridor" in visible:
            add(lines(frame.corridor), "corridor", 2.)
            triangles = tuple(triangle for shape in frame.corridor for group in shape.triangles for triangle in group)
            add(triangles, "corridor", 1., surface=True)
        if "camera" in visible and frame.camera is not None:
            edges, _ = visible_world_lines(scene, frame.camera)
            if edges:
                for color, width in (((.12, .09, .20), 3.2), (SPATIAL_COLORS["camera"], 1.6)):
                    actor = overlay_lines(edges, color, width*self.scale)
                    overlay.AddViewProp(actor)
                    self.dynamic.append((overlay, actor))

    def frame(self,pose,trajectory,label,elapsed=0.,fpv_pose=None, spatial=None):
        self.update_range_colors(pose[:3, 3])
        for renderer,actor in self.dynamic: renderer.RemoveActor(actor)
        self.dynamic=[]
        self.update_scan(spatial.scan if spatial else None)
        self.label.SetInput(('SIMULATION' if label.startswith('SYNTHETIC') else '') if self.clean
                            else label+'\nX red / Y green / Z blue | metres')
        for index,(scene,overlay) in enumerate(self.views):
            mode = self.modes[index]
            zoom = self.view_zooms[index]
            position,target,up = view_camera(pose, mode, self.camera_args, self.inset_args, elapsed, fpv_pose, zoom=zoom)
            cam=scene.GetActiveCamera(); cam.SetPosition(*position); cam.SetFocalPoint(*target)
            cam.SetViewUp(*up); cam.SetClippingRange(.035,1000)
            if mode in FPV_MODES:
                cam.SetViewAngle(zoomed_fov(self.fov, zoom))
            visible = self.visible_overlays[index]
            if "actual" in visible and len(trajectory)>1:
                if self.slam_view:
                    parts, _ = visible_world_lines(scene, (trajectory,))
                    if parts:
                        for color, width in (((.09, .085, .065), self.trajectory_width + 2 * self.scale),
                                             (self.trajectory_color, self.trajectory_width)):
                            artist = overlay_lines(parts, color, width)
                            overlay.AddViewProp(artist)
                            self.dynamic.append((overlay, artist))
                else:
                    actor=line_actor(trajectory,self.trajectory_color,self.trajectory_width)
                    scene.AddActor(actor); self.dynamic.append((scene,actor))
            if spatial is not None:
                self.add_spatial(scene, overlay, spatial, visible)
            viewport_width, viewport_height = self.windows[index].GetSize()
            screen_space = mode in FPV_MODES
            if screen_space:
                origin = np.array([.095 * viewport_width, .17 * viewport_height, 0.])
                length = np.clip(.065 * min(viewport_width, viewport_height), 18 * self.scale, 54 * self.scale)
                # Use the measured, unsmoothed body rotation even for a stabilized camera.
                axes = ATTITUDE_PROJECTION @ pose[:3, :3]
            else:
                origin = pose[:3, 3]
                distance = np.linalg.norm(position - origin)
                min_length = self.axis_pixels * 2 * distance * np.tan(np.deg2rad(cam.GetViewAngle()/2)) / viewport_height
                length = np.clip(self.axis, min_length, 2 * min_length)
                axes = pose[:3, :3]
            for j,color in reversed(list(enumerate([(1,.13,.12),(.15,1,.27),(.15,.43,1)]))):
                points=np.array([origin, origin + axes[:, j] * length])
                groups = (points,) if screen_space else visible_world_lines(scene, (points,))[0]
                if not groups:
                    continue
                for c,w in (((.01,.015,.025), self.axis_width + 2*self.scale), (color, self.axis_width)):
                    actor=overlay_lines(groups,c,w,screen_space=screen_space)
                    overlay.AddViewProp(actor); self.dynamic.append((overlay,actor))
        # Independent EDL framebuffers avoid driver-dependent viewport clearing.
        # Both use exactly the same cloud, trajectory and timestamped pose above.
        images=[]
        for window in self.windows:
            window.Render()
            capture=vtk.vtkWindowToImageFilter(); capture.SetInput(window)
            capture.SetInputBufferTypeToRGBA(); capture.ReadFrontBufferOff()
            capture.ShouldRerenderOff(); capture.Update()
            w,h=window.GetSize()
            images.append(np.flipud(vtk_to_numpy(capture.GetOutput().GetPointData().GetScalars()).reshape(h,w,4)).copy())
        result = Image.fromarray(images[0])
        color = result.convert("RGB")
        alpha = result.getchannel("A")
        if self.view_modes is None:
            inset = Image.fromarray(images[1])
            corner = round(self.width * .66), round(self.height * .04)
            mask = inset.getchannel("A")
            if self.clean:
                mask = ImageChops.multiply(mask, rounded_mask(inset.size, 18 * self.scale))
            color.paste(inset.convert("RGB"), corner, mask)
            inset_alpha = Image.new("L", result.size)
            inset_alpha.paste(mask, corner)
            alpha = ImageChops.screen(alpha, inset_alpha)
        if self.scale > 1:
            color = color.resize(self.output_size, Image.Resampling.LANCZOS)
        if self.edl:
            color = color.point(EDL_TONES)
        if self.clean:
            color.putalpha(alpha.resize(self.output_size, Image.Resampling.LANCZOS)
                           if self.scale > 1 else alpha)
        if self.view_modes is not None:
            view_images = {}
            for mode, captured in zip(self.view_modes, images[1:]):
                view = Image.fromarray(captured)
                if self.edl:
                    corrected = view.convert("RGB").point(EDL_TONES)
                    corrected.putalpha(view.getchannel("A"))
                    view = corrected
                view_images[mode] = view
            return np.asarray(color), view_images
        return np.asarray(color)

    def close(self):
        for window in self.windows: window.Finalize()
