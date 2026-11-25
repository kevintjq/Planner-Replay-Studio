"""Two synchronized VTK EDL viewports, plus foreground body RGB axes."""
import numpy as np
from PIL import Image
import vtk
from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray, vtk_to_numpy
from .geometry import camera


# Lift EDL midtones while preserving the dark background and white overlays.
EDL_TONES = [v if v <= 14 else round(14 + 241 * ((v - 14) / 241) ** .66)
             for v in range(256)] * 3


def point_actor(xyz, rgb, size):
    poly=vtk.vtkPolyData()
    pts=vtk.vtkPoints(); pts.SetData(numpy_to_vtk(np.ascontiguousarray(xyz),deep=True)); poly.SetPoints(pts)
    cells=vtk.vtkCellArray()
    offsets = np.arange(len(xyz) + 1, dtype=np.int64)
    cells.SetData(numpy_to_vtkIdTypeArray(offsets, deep=True),
                  numpy_to_vtkIdTypeArray(offsets[:-1], deep=True))
    poly.SetVerts(cells)
    poly.GetPointData().SetScalars(numpy_to_vtk(np.ascontiguousarray(rgb,dtype='u1'),deep=True))
    mapper=vtk.vtkPolyDataMapper(); mapper.SetInputData(poly)
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


def overlay_line(points,color,width):
    poly=vtk.vtkPolyData(); pts=vtk.vtkPoints()
    pts.SetData(numpy_to_vtk(np.ascontiguousarray(points,dtype=float),deep=True)); poly.SetPoints(pts)
    cells=vtk.vtkCellArray(); cells.InsertNextCell(len(points))
    for i in range(len(points)): cells.InsertCellPoint(i)
    poly.SetLines(cells)
    coordinate=vtk.vtkCoordinate(); coordinate.SetCoordinateSystemToWorld()
    mapper=vtk.vtkPolyDataMapper2D(); mapper.SetInputData(poly); mapper.SetTransformCoordinate(coordinate)
    actor=vtk.vtkActor2D(); actor.SetMapper(mapper)
    actor.GetProperty().SetColor(*color); actor.GetProperty().SetLineWidth(width)
    return actor


class Renderer:
    def __init__(self,width=1280,height=720,edl=True,axis=.48,axis_pixels=28,point_size=2,
                 distance=3.8,camera_height=1.35,side=0.,fov=65,inset_distance=10,inset_height=6.7,
                 *, supersample=1, cloud_color=None, trajectory_color=(1,.73,.18), trajectory_width=3):
        self.output_size = (width, height)
        self.scale = supersample
        self.edl = edl
        self.width, self.height = width * supersample, height * supersample
        self.axis, self.axis_pixels = axis, axis_pixels * supersample
        self.point_size = point_size * supersample
        self.cloud_color = cloud_color
        self.trajectory_color = trajectory_color
        self.trajectory_width = trajectory_width * supersample
        self.camera_args=(distance,camera_height,side)
        self.inset_args=(inset_distance,inset_height,0.)
        self.windows=[]
        self.views=[]
        self.cloud_actors=[]
        self.dynamic=[]
        for index in range(2):
            window=vtk.vtkRenderWindow()
            window.SetSize(self.width if index == 0 else round(self.width * .32),
                           self.height if index == 0 else round(self.height * .33))
            window.SetOffScreenRendering(1); window.SetMultiSamples(0); window.SetNumberOfLayers(2)
            self.windows.append(window)
            scene=vtk.vtkRenderer(); scene.SetViewport(0,0,1,1); scene.SetLayer(0)
            scene.SetBackground(.018,.028,.047); scene.SetPreserveColorBuffer(False)
            if edl:
                basic=vtk.vtkRenderStepsPass(); effect=vtk.vtkEDLShading(); effect.SetDelegatePass(basic)
                scene.SetPass(effect)
            overlay=vtk.vtkRenderer(); overlay.SetViewport(0,0,1,1); overlay.SetLayer(1)
            overlay.SetActiveCamera(scene.GetActiveCamera()); overlay.SetPreserveDepthBuffer(False)
            scene.GetActiveCamera().SetViewAngle(fov)
            window.AddRenderer(scene); window.AddRenderer(overlay)
            self.views.append((scene,overlay))
        self.label=vtk.vtkTextActor(); self.label.SetPosition(22*self.scale,22*self.scale)
        self.label.GetTextProperty().SetFontSize(17*self.scale); self.label.GetTextProperty().SetColor(.9,.94,1)
        self.views[0][1].AddViewProp(self.label)
        self.inset_label=vtk.vtkTextActor()
        self.inset_label.SetPosition(10*self.scale,10*self.scale); self.inset_label.GetTextProperty().SetFontSize(12*self.scale)
        self.views[1][1].AddViewProp(self.inset_label)
        self.inset_label.SetInput('OBLIQUE FOLLOW')

    def update_cloud(self,cloud):
        for scene,actor in self.cloud_actors: scene.RemoveActor(actor)
        self.cloud_actors=[]
        rgb = cloud.rgb if self.cloud_color is None else np.broadcast_to(self.cloud_color, cloud.rgb.shape)
        for scene, _ in self.views:
            # Each mapper owns GPU buffers for its own OpenGL context.
            actor = point_actor(cloud.xyz, rgb, self.point_size)
            scene.AddActor(actor)
            self.cloud_actors.append((scene, actor))

    def frame(self,pose,trajectory,label):
        for renderer,actor in self.dynamic: renderer.RemoveActor(actor)
        self.dynamic=[]
        self.label.SetInput(label+'\nX red / Y green / Z blue | metres')
        for index,(scene,overlay) in enumerate(self.views):
            args=self.camera_args if index==0 else self.inset_args
            position,target=camera(pose,*args,lookahead=1.5 if index==0 else 0.)
            cam=scene.GetActiveCamera(); cam.SetPosition(*position); cam.SetFocalPoint(*target)
            cam.SetViewUp(0,0,1); cam.SetClippingRange(.03,1000)
            if len(trajectory)>1:
                actor=line_actor(trajectory,self.trajectory_color,self.trajectory_width)
                scene.AddActor(actor); self.dynamic.append((scene,actor))
            viewport_height=self.height*(1 if index==0 else .33)
            distance=np.linalg.norm(position-pose[:3,3])
            min_length=self.axis_pixels*2*distance*np.tan(np.deg2rad(cam.GetViewAngle()/2))/viewport_height
            length=max(self.axis,min_length)
            origin=pose[:3,3]
            for j,color in reversed(list(enumerate([(1,.13,.12),(.15,1,.27),(.15,.43,1)]))):
                points=np.array([origin,origin+pose[:3,j]*length])
                for c,w in [((.01,.015,.025),7),(color,3.5)]:
                    actor=overlay_line(points,c,w*self.scale); overlay.AddViewProp(actor); self.dynamic.append((overlay,actor))
        # Independent EDL framebuffers avoid driver-dependent viewport clearing.
        # Both use exactly the same cloud, trajectory and timestamped pose above.
        images=[]
        for window in self.windows:
            window.Render()
            capture=vtk.vtkWindowToImageFilter(); capture.SetInput(window)
            capture.SetInputBufferTypeToRGB(); capture.ReadFrontBufferOff()
            capture.ShouldRerenderOff(); capture.Update()
            w,h=window.GetSize()
            images.append(np.flipud(vtk_to_numpy(capture.GetOutput().GetPointData().GetScalars()).reshape(h,w,3)).copy())
        result,inset=images
        h,w=inset.shape[:2]; top=round(self.height*.04); left=round(self.width*.66)
        border = 2 * self.scale
        result[max(0, top-border):top+h+border, max(0, left-border):left+w+border] = [75,92,115]
        result[top:top+h,left:left+w]=inset
        image = Image.fromarray(result)
        if self.scale > 1:
            image = image.resize(self.output_size, Image.Resampling.LANCZOS)
        if self.edl:
            image = image.point(EDL_TONES)
        return np.asarray(image)

    def close(self):
        for window in self.windows: window.Finalize()
