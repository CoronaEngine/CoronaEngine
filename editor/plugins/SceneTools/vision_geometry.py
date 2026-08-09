"""Pure Vision geometry, transform and primitive mesh calculations."""

import math

from .vision_document import vision_shape_params


def flatten_matrix4x4(value):
    if isinstance(value, list) and len(value) == 16:
        try:
            return [float(item) for item in value]
        except (TypeError, ValueError):
            return None
    if isinstance(value, list) and len(value) == 4 and all(
        isinstance(row, list) and len(row) == 4 for row in value
    ):
        try:
            return [float(item) for row in value for item in row]
        except (TypeError, ValueError):
            return None
    return None


def vector_length(vec):
    return math.sqrt(sum(component * component for component in vec))


def clean_near_zero(value):
    return 0.0 if abs(value) < 1e-9 else value


def aabb_center_and_max_axis(vertices):
    if not vertices:
        return [0.0, 0.0, 0.0], 1.0
    mins = [min(vertex[i] for vertex in vertices) for i in range(3)]
    maxs = [max(vertex[i] for vertex in vertices) for i in range(3)]
    center = [(mins[i] + maxs[i]) * 0.5 for i in range(3)]
    max_axis = max(maxs[i] - mins[i] for i in range(3))
    return center, max_axis if max_axis > 1e-8 else 1.0


def matrix4x4_to_corona_trs(matrix):
    # Vision and Corona use opposite Z handedness. Convert object matrices with
    # F * M * F, matching the C++ built-in Vision geometry adapter.
    position = [matrix[12], matrix[13], -matrix[14]]
    columns = [
        [matrix[0], matrix[1], -matrix[2]],
        [matrix[4], matrix[5], -matrix[6]],
        [-matrix[8], -matrix[9], matrix[10]],
    ]
    scale = [vector_length(column) for column in columns]
    if any(component <= 1e-8 for component in scale):
        return {"position": position, "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}

    r00, r10, r20 = [columns[0][i] / scale[0] for i in range(3)]
    r01, r11, r21 = [columns[1][i] / scale[1] for i in range(3)]
    r02, r12, r22 = [columns[2][i] / scale[2] for i in range(3)]

    sin_y = max(-1.0, min(1.0, -r20))
    y = math.asin(sin_y)
    cos_y = math.cos(y)
    if abs(cos_y) > 1e-6:
        x = math.atan2(r21, r22)
        z = math.atan2(r10, r00)
    else:
        x = math.atan2(-r12, r11)
        z = 0.0

    return {"position": position, "rotation": [x, y, z], "scale": scale}


def vision_transform_matrix(shape: dict):
    params = vision_shape_params(shape)
    transform = params.get("transform") if isinstance(params.get("transform"), dict) else {}
    transform_params = transform.get("param") if isinstance(transform.get("param"), dict) else transform
    transform_type = str(transform.get("type") or "matrix4x4").lower()
    if transform_type != "matrix4x4":
        return None
    return flatten_matrix4x4(transform_params.get("matrix4x4"))


def apply_vision_matrix_to_corona(matrix, point):
    if not matrix:
        x, y, z = point
    else:
        px, py, pz = point
        x = px * matrix[0] + py * matrix[4] + pz * matrix[8] + matrix[12]
        y = px * matrix[1] + py * matrix[5] + pz * matrix[9] + matrix[13]
        z = px * matrix[2] + py * matrix[6] + pz * matrix[10] + matrix[14]
    return [x, y, -z]


def apply_corona_trs_to_point(transform: dict, point):
    sx, sy, sz = transform.get("scale", [1.0, 1.0, 1.0])
    rx, ry, rz = transform.get("rotation", [0.0, 0.0, 0.0])
    tx, ty, tz = transform.get("position", [0.0, 0.0, 0.0])
    x = point[0] * sx
    y = point[1] * sy
    z = point[2] * sz

    cos_x = math.cos(rx)
    sin_x = math.sin(rx)
    y, z = y * cos_x - z * sin_x, y * sin_x + z * cos_x

    cos_y = math.cos(ry)
    sin_y = math.sin(ry)
    x, z = x * cos_y + z * sin_y, -x * sin_y + z * cos_y

    cos_z = math.cos(rz)
    sin_z = math.sin(rz)
    x, y = x * cos_z - y * sin_z, x * sin_z + y * cos_z

    return [x + tx, y + ty, z + tz]


def extract_vision_shape_transform(shape: dict) -> dict:
    params = vision_shape_params(shape)
    transform = params.get("transform") if isinstance(params.get("transform"), dict) else {}
    transform_params = transform.get("param") if isinstance(transform.get("param"), dict) else transform
    transform_type = str(transform.get("type") or "matrix4x4").lower()

    position = [0.0, 0.0, 0.0]
    rotation = [0.0, 0.0, 0.0]
    scale = [1.0, 1.0, 1.0]

    if transform_type == "matrix4x4":
        matrix = flatten_matrix4x4(transform_params.get("matrix4x4"))
        if matrix:
            return matrix4x4_to_corona_trs(matrix)
    elif transform_type == "trs":
        t = transform_params.get("t")
        s = transform_params.get("s")
        t = t if isinstance(t, (list, tuple)) and len(t) >= 3 else None
        s = s if isinstance(s, (list, tuple)) and len(s) >= 3 else None
        if t:
            position = [float(t[0]), float(t[1]), -float(t[2])]
        if s:
            scale = [float(s[0]), float(s[1]), float(s[2])]
    elif transform_type == "euler":
        t = transform_params.get("position")
        t = t if isinstance(t, (list, tuple)) and len(t) >= 3 else None
        if t:
            position = [float(t[0]), float(t[1]), -float(t[2])]
        try:
            rotation = [
                float(transform_params.get("pitch", 0.0)),
                -float(transform_params.get("yaw", 0.0)),
                -float(transform_params.get("roll", 0.0)),
            ]
        except (TypeError, ValueError):
            rotation = [0.0, 0.0, 0.0]

    return {"position": position, "rotation": rotation, "scale": scale}


def vision_primitive_vertices(shape: dict, shape_type: str):
    params = vision_shape_params(shape)
    if shape_type == "quad":
        width = float(params.get("width", 1.0))
        height = float(params.get("height", 1.0))
        hw = width * 0.5
        hh = height * 0.5
        return [
            [hw, 0.0, hh], [hw, 0.0, -hh], [-hw, 0.0, hh], [-hw, 0.0, -hh]
        ], [[1, 2, 3], [3, 2, 4]]
    if shape_type == "cube":
        x = float(params.get("x", params.get("width", 1.0)))
        y = float(params.get("y", params.get("height", 1.0)))
        z = float(params.get("z", params.get("depth", 1.0)))
        y = x if y == 0.0 else y
        z = y if z == 0.0 else z
        sx = x * 0.5
        sy = y * 0.5
        sz = z * 0.5
        return [
            [-sx, -sy, -sz], [sx, -sy, -sz], [sx, sy, -sz], [-sx, sy, -sz],
            [-sx, -sy, sz], [sx, -sy, sz], [sx, sy, sz], [-sx, sy, sz],
        ], [
            [1, 2, 3, 4], [5, 8, 7, 6], [1, 5, 6, 2], [2, 6, 7, 3],
            [3, 7, 8, 4], [4, 8, 5, 1],
        ]
    if shape_type == "sphere":
        radius = float(params.get("radius", 1.0))
        theta_div = max(3, int(params.get("sub_div", 60)))
        phi_div = 2 * theta_div
        vertices = [[0.0, radius, 0.0]]
        for i in range(1, theta_div):
            v = float(i) / theta_div
            theta = math.pi * v
            y = radius * math.cos(theta)
            ring_radius = radius * math.sin(theta)
            for j in range(phi_div):
                u = float(j) / phi_div
                phi = u * math.tau
                vertices.append([
                    math.cos(phi) * ring_radius,
                    y,
                    math.sin(phi) * ring_radius,
                ])
        vertices.append([0.0, -radius, 0.0])

        faces = []
        for i in range(phi_div):
            faces.append([1, ((i + 1) % phi_div) + 2, i + 2])

        for i in range(theta_div - 2):
            vert_start = 2 + i * phi_div
            for j in range(phi_div):
                current = vert_start + j
                next_vertex = vert_start + ((j + 1) % phi_div)
                below = current + phi_div
                below_next = next_vertex + phi_div
                faces.append([current, next_vertex, below])
                faces.append([next_vertex, below_next, below])

        bottom = len(vertices)
        last_ring = 2 + (theta_div - 2) * phi_div
        for i in range(phi_div):
            current = last_ring + i
            next_vertex = last_ring + ((i + 1) % phi_div)
            faces.append([bottom, next_vertex, current])
        return vertices, faces
    return [], []


def vision_primitive_world_vertices(shape: dict, local_vertices):
    matrix = vision_transform_matrix(shape)
    if matrix:
        return [apply_vision_matrix_to_corona(matrix, vertex) for vertex in local_vertices]

    transform = extract_vision_shape_transform(shape)
    return [
        apply_corona_trs_to_point(transform, [vertex[0], vertex[1], -vertex[2]])
        for vertex in local_vertices
    ]


def rotate_corona_vector(vec, rotation):
    x, y, z = vec
    rx, ry, rz = rotation

    cos_x = math.cos(rx)
    sin_x = math.sin(rx)
    y, z = y * cos_x - z * sin_x, y * sin_x + z * cos_x

    cos_y = math.cos(ry)
    sin_y = math.sin(ry)
    x, z = x * cos_y + z * sin_y, -x * sin_y + z * cos_y

    cos_z = math.cos(rz)
    sin_z = math.sin(rz)
    x, y = x * cos_z - y * sin_z, x * sin_z + y * cos_z
    return [x, y, z]


def corona_trs_to_vision_matrix4x4(transform: dict):
    position = transform.get("position", [0.0, 0.0, 0.0])
    rotation = transform.get("rotation", [0.0, 0.0, 0.0])
    scale = transform.get("scale", [1.0, 1.0, 1.0])

    corona_columns = [
        rotate_corona_vector([scale[0], 0.0, 0.0], rotation) + [0.0],
        rotate_corona_vector([0.0, scale[1], 0.0], rotation) + [0.0],
        rotate_corona_vector([0.0, 0.0, scale[2]], rotation) + [0.0],
        [position[0], position[1], position[2], 1.0],
    ]

    vision_columns = []
    for col_index, column in enumerate(corona_columns):
        vision_column = []
        for row_index, value in enumerate(column):
            if row_index == 2:
                value = -value
            if col_index == 2:
                value = -value
            vision_column.append(clean_near_zero(float(value)))
        vision_columns.append(vision_column)
    return vision_columns
