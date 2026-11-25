"""Two synchronized VTK EDL viewports, plus foreground body RGB axes."""
import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy
from .geometry import camera


def point_actor(xyz, rgb, size):
    poly=vtk.vtkPolyData()
    pts=vtk.vtkPoints(); pts.SetData(numpy_to_vtk(np.ascontiguousarray(xyz),deep=True)); poly.SetPoints(pts)
    cells=vtk.vtkCellArray()
    for i in range(len(xyz)):
        cells.InsertNextCell(1); cells.InsertCellPoint(i)
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
                 distance=3.8,camera_height=1.35,side=0.,fov=65,inset_distance=10,inset_height=6.7):
        self.width,self.height=width,height
        self.axis,self.axis_pixels,self.point_size=axis,axis_pixels,point_size
        self.camera_args=(distance,camera_height,side)
        self.inset_args=(inset_distance,inset_height,0.)
        self.windows=[]
        self.views=[]
        self.cloud_actors=[]
        self.dynamic=[]
        for index,viewport in enumerate([(0,0,1,1),(.66,.63,.98,.96)]):
            window=vtk.vtkRenderWindow()
            window.SetSize(width if index==0 else round(width*.32),height if index==0 else round(height*.33))
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
        self.label=vtk.vtkTextActor(); self.label.SetPosition(22,22)
        self.label.GetTextProperty().SetFontSize(17); self.label.GetTextProperty().SetColor(.9,.94,1)
        self.views[0][1].AddViewProp(self.label)
        self.inset_label=vtk.vtkTextActor(); self.inset_label.SetInput('OBLIQUE FOLLOW | 34 deg')
        self.inset_label.SetPosition(10,10); self.inset_label.GetTextProperty().SetFontSize(12)
        self.views[1][1].AddViewProp(self.inset_label)
        self.inset_label.SetInput('OBLIQUE FOLLOW')

    def update_cloud(self,cloud):
        for scene,actor in self.cloud_actors: scene.RemoveActor(actor)
        self.cloud_actors=[]
        actor=point_actor(cloud.xyz,cloud.rgb,self.point_size)
        for scene,_ in self.views:
            scene.AddActor(actor); self.cloud_actors.append((scene,actor))

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
                actor=line_actor(trajectory,(1,.73,.18),3)
                scene.AddActor(actor); self.dynamic.append((scene,actor))
            viewport_height=self.height*(1 if index==0 else .33)
            distance=np.linalg.norm(position-pose[:3,3])
            min_length=self.axis_pixels*2*distance*np.tan(np.deg2rad(cam.GetViewAngle()/2))/viewport_height
            length=max(self.axis,min_length)
            origin=pose[:3,3]
            for j,color in reversed(list(enumerate([(1,.13,.12),(.15,1,.27),(.15,.43,1)]))):
                points=np.array([origin,origin+pose[:3,j]*length])
                for c,w in [((.01,.015,.025),7),(color,3.5)]:
                    actor=overlay_line(points,c,w); overlay.AddViewProp(actor); self.dynamic.append((overlay,actor))
        # Independent EDL framebuffers avoid driver-dependent viewport clearing.
        # Both use exactly the same cloud, trajectory and timestamped pose above.
        images=[]
        for window in self.windows:
            window.Render()
            capture=vtk.vtkWindowToImageFilter(); capture.SetInput(window)
            capture.SetInputBufferTypeToRGB(); capture.ReadFrontBufferOff(); capture.Update()
            w,h=window.GetSize()
            images.append(np.flipud(vtk_to_numpy(capture.GetOutput().GetPointData().GetScalars()).reshape(h,w,3)).copy())
        result,inset=images
        h,w=inset.shape[:2]; top=round(self.height*.04); left=round(self.width*.66)
        result[top-2:top+h+2,left-2:left+w+2]=[75,92,115]
        result[top:top+h,left:left+w]=inset
        return result

    def close(self):
        for window in self.windows: window.Finalize()
