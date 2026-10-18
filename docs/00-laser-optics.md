# Laser Optics Simulator

I want to modify the raytracer to better simulate physical optical experiments, and particularly but not restricted to laser optics. I want to be able to test and simulate physical experiments in the laser lab where I work. Ideally, I would be able to preview how a given setup deviates from the theoretically ideal case due to effects like astigmatism, clipping, misalignment etc.

Take inspiration from the software FemtoPro (https://www.uni-wuerzburg.de/en/femtopro/). Do not attempt to merely copy its functionality, but take its approach as inspiration. The page of features (https://www.uni-wuerzburg.de/en/femtopro/features/) is a good starting point. However I do NOT want the following features:

- VR support
- tutorials or educational features
- multiplayer

The publications related to femtopro are downloaded to `/Users/kobihall/Downloads/FemtoPro_Papers`, read them (and access any required figures from the web if needed) to get an idea of the technical decisions made. 

The following available optical elements are listed below. This defines the geometry of the elements, but the material properties (dielectric function, R/T curves, coatings, etc.) can be further specified.

- planar mirrors (with diameters 1/2", 1", and 2")
- spherical concave mirrors (with real time tunable radius of curvature) (with diameters 1/2", 1", and 2")
- off axis parabolic mirror (with real time tunable focal length) (with diameters 1/2", 1", and 2")
- circular planar filters (with diameters 1/2", 1", and 2", user defined thickness) 
- square planar filters (arbitrarily large, minimum 50mm, user defined thickness)
- irises (internal diameter 8, 12, 15, 20, 25, 36, and 50mm)
- plate beamsplitters (with diameters 1/2", 1", and 2", user defined thickness) (both polarizing and nonpolarizing)
- cube beamsplitters (with side lengths 1/2", 1", and 2") (both polarizing and nonpolarizing) 
- spherical lenses (with diameters 1/2", 1", and 2") (Plano-Convex, Bi-Convex, Plano-Concave, or Bi-Concave) (user defined thickness and radii)
- right angle prisms (with leg dimensions 10, 25, and 60mm)
- equilateral dispersion prisms (with side lengths 10, 25, and 50mm)
- plate wedge prisms (arbitrarily large, minimum 50mm) (with angles 2, 4, 6, and 10 degrees) 
- round wedge prisms (with diameters 1/2", 1", and 2") (with angles 2, 4, 6, and 10 degrees) 

If there are any uncertain paramters for these optical elements, don't make assumptions, instead reference the thorlabs website and ask me.

Each of these elements can be translated and rotated freely using the standard scene editor transform gizmo. This base transformation should determine the placement of the corresponding optical mount (the coarse placement of a kinematic mount for example). The translation should be able to be toggled between continuous movement, and locking to the breadboard holes seperated by 25mm. The chosen mount may also have further fine adjustment (two or three fine adjustment screws in a kinematic mount, or rotation of a QWP mount). This fine adjustment should be a separate setting inside the object settings beyond the transform gizmo.  

# Beam Axis Object

I want to implement a "beam axis" object that allows the user to manually add optical elements in a graph based fashion. Mirrors, lenses, irises, etc. should be linearly arranged in the graph structure. Beam splitters, acousto optic modulators, nonlinear crystals, etc. can have multiple child nodes.

The graph based structure should not entirely dictate the physical placement of the objects, the user still decides the placement and orientation of the optical elements via the standard transform gizmo in the scene editor. However, there should be an option for a "partially locked" transform option that allows for transforming multiple objects along a single degree of freedom. For example, if there is a laser source + a 90 degree turning mirror + a number of lenses and filters arranged linearly, and the user wants to rotate the turning mirror to 60 degrees instead, there should be a way with this locked option to rotate the mirror and all the child optics rotate with it along the mirror's axis of rotation. Another example is two mirrors + an arbitrary optic after the second mirror; if the user wants to translate the second mirror using this locked option, then the first mirror should accordingly rotate to direct the beam onto the second mirror and the second mirror should rotate to direct the beam onto the arbitrary next optic. Essentially, this partially locked transform feature should allow a user to select the kind of transformation (and thus corresponding degree of freedom) and also select the dependent transformations so that spatial beam path or alignment remains as static as possible. For a selected number of options, a certain movement should be deterministic, so decide the method and user options that make most sense to this end. 

There should also be an option to determine the extent to which the graph structure can reduce scene traversal and intersection queries. In the maximal case there should be no traversal of the full scene/acceleration structure; the next ray only checks the child nodes in the beam axis (with multiple child nodes, as in a beam splitter for instance, chosen stochastically). However, in a proper render, I wouldn't exculesively trace the next element in the graph, as I would want to consider accidentally clipping the beam or stray reflections/backreflections/ghosts. Some intermediate option could potentially use some sort of weighting. Look into existing implementations and various ways to use the beam axis object in the ray tracing algorithm.

# File Import/Export

This node based beam axis should also be able to support importing from zeemax `.zmx` files and potential other representations of optical data. This should not be a two way import/export, this "beam axis" object need not be structured in the same way as zeemax files, thus there should be some amount of interpretation. One immediate discrepancy that I can think of is that zeemax files structure data in a surface-by-surface manner, whereas my raytracer stores individual objects with multiples possible intersection surfaces. There may not always be a clean translation between the two representations, so it's okay to resort to an import failure rather than overcomplicating this feature.

The @cameras.md plan also includes the option for a realistic camera option, as specified by PBRT's realistic camera section and lens `.dat` files. You can update the cameras plan to instead use this beam axis object and add an interpreter for these lens files.

On the topic of support for external file systems, there should be a way to verify against other more trusted simulators. Look into the possibility and practicality of saving outputs to ZBF or ZBF2 formats, or maybe some other standard or some proprietary way to save output data that is more that just an image.

# Raster View Preview

There should be a more visually descriptive scene in the raster preview window. A laser beam (in the raster window) should be represented by at most, three cylindrical sections per propagation between elements. The radius of these sections should approximate the beam radius $w(z)$ along the propagation path. If a focal spot is not inside a single propagation section (the section is purely diverging/converging), then the raster preview geometry should be only a conical cylinder (connecting two disks with radii $\approx w(z_1)$ and $\approx w(z_2)$). If a focal spot is inside a single propagation region, there should be two conical cylinders, with a straight cylinder in the center. The central cylinder should have radius $w_0$ (beam waist) and length $z_R$ (rayleigh length). This beam preview should be raytraced using the existing framework, but just submit a single ray per propagation segment to determine the geometry of the beam axis.

In the raster preview, individual optical components should also be represented by more complex 3D models. I already have STEP files downloaded from thorlabs in the `assets/optical_models/` folder. These models should be treated as fixed resource models, and not to be swapped or chosen in real time (similar to how the sphere triangle model in the raster preview represents a perfect sphere hittable in the raytracing scene). Therefore, it's okay to have some amount of this geometry logic hardcoded, and not loaded in modularly like the other scene models. Furthermore, these models themselves should not be saved in the scene description, but instead always generated from the given optical element.

All of these optical components should connect to a breadboard with the face at y=0. The breadboard model is `assets/optical_models/Breadboard-B3030A.step`. This model is the most complicated internally (thus redundant geometry), so just create a mesh that looks identical from the outside. This sets the breadboard hole seperation of 25mm and where the mounts screw into.

The connection to the breadboard should always include the pair `assets/optical_models/ePost_holder_clamp-CF125C_M.STEP` and `assets/optical_models/ePost_holder-PH50E_M.step`, as well as a procedurally generated post. The center of the post holder should align with the mounting screw hole of whatever mount holds the optical element. There are currently the following types of component mounts:
- kinematic mounts for all mirrors and other circular optics
- fixed mounts for lenses, circular filters, and thin circular crystals
- fixed rotation mounts for circular filters with a preferred axis (polarizers, HWP, QWP, etc.), round beamsplitters
- prism mounts for cube beamsplitters and all non round prisms
- a filter mount for square filters and crystals
- all-in-one iris assembly mounts

The available kinematic mounts are
| filename | optic diameter |
|-|-|
| `Kinematic_mount-KM05T_M.step` | 12.7 mm (1/2") |
| `Kinematic_mount-KM100.step` | 25.4 mm (1") |
| `Kinematic_mount-KM100.step` | 50.8 mm (2") |

Planar mirrors themselves should be represented by the following models
| filename | mirror diameter |
|-|-|
| `Mirror_blank-PF05-03.step` | 12.7 mm (1/2") |
| `Mirror_blank-PF10-03.step` | 25.4 mm (1") |
| `Mirror_blank-PF20-03.step` | 50.8 mm (2") |

An example mirror assembly (without the cylindrical post) can be found in the freecad assembly file `assets/optical_models/example mirror.FCStd`.
Nonplanar mirrors can temporarily be represented by the above models as well.

The available fixed mounts are

| filename | optic diameter |
|-|-|
| `Fixed_mount-LMR05_M.step` | 12.7 mm (1/2") |
| `Fixed_mount-LMR1_M.step` | 25.4 mm (1") |
| `Fixed_mount-LMR2_M.step` | 50.8 mm (2") |

The available fixed rotation mounts are

| filename | optic diameter |
|-|-|
| `Rotation_mount-RSP05_M.step` | 12.7 mm (1/2") |
| `Rotation_mount-RSP1_M.step` | 25.4 mm (1") |
| `Rotation_mount-RSP2_M.step` | 50.8 mm (2") |

There is only one model for prism mounts `Prism_mount-KM200PM_M.step`. There is also only one model for mounting square filters `Filter_mount-SFH3.step`

The available iris models are

| filename | internal diameter (mm) | iris opening range (mm) |
|-|-|-|
| `Iris-ID8_M.step` | 8 | 0.8-8 |
| `Iris-ID12_M.step` | 12 | 1-12 |
| `Iris-ID15_M.step` | 15 | 1.2-15 |
| `Iris-ID20_M.step` | 20 | 1.2-20 |
| `Iris-ID25_M.step` | 25 | 1.4-25 |
| `Iris-ID36_M.step` | 36 | 1.9-36 |
| `Iris-ID50_M.step` | 50 | 2.5-50 |

Take all of these STEP files, convert them to triangle meshes, add connection nodes, and clean them up. By clean them up, I mean remove any text geometry, hidden detail on the interiors, and screw threads. Turn text geometry into a flat texture if necessary. Also by connection nodes, I mean points on the model that should "mate" with points on other models (eg. center of the screw hole in the kinematic mount that defines the axis to center the post, post holder, and post holder clamp around).


# Verification/Goals

I want to be able to currently build:
- real DSLR camera lens system (35mm prime lens for example)
- telescope
- microscope
- gaussian laser beam propagation through system of mirrors and lenses
- off axis parabola mirror focal spot

Once the spectral renderer is implemented, I want to be able to build:
- prism diffraction
- lens chromatic abberation
- XUV optics (gratings, toroidal mirror)
- dichroic mirrors

Once polarization is implemented, I want to be able to build:
- HWP and QWP polarization rotation and polarizing beam splitters to divide power between two paths
- Brewster window
- birefringent crystals

Once the dyadic green's function for wave ray tracing is implemented, I want to be able to build:
- double slit experiment
- iris ring diffraction (airy disk) for beam walking alignment 
- various interferometers
- diffraction effects from finitely sized optical components