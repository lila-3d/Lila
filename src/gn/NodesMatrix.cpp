#include "gn/NodesCommon.h"

#include <glm/gtc/matrix_access.hpp>

namespace lila::gn::nodes::matrixnodes
{
using D4 = glm::dvec4;
using M4 = glm::dmat4;

D3 translationOf(const M4& m) { return D3(m[3]); }
D3 scaleOf(const M4& m) { return {glm::length(D3(m[0])) * (glm::determinant(glm::dmat3(m)) < 0.0 ? -1.0 : 1.0), glm::length(D3(m[1])), glm::length(D3(m[2]))}; }

D3 rotationOf(const M4& m)
{
    D3 s = scaleOf(m);
    glm::dmat3 r(m);
    for (int k = 0; k < 3; ++k)
        if (std::abs(s[k]) > 1e-12)
            r[k] /= s[k];
    return matrixToEuler(r);
}

/** "c0r1": column 0, row 1, as Combine and Separate Matrix name their sockets. */
std::string cell(int c, int r) { return "c" + std::to_string(c) + "r" + std::to_string(r); }

void buildNodes()
{
    std::vector<SocketDecl> cellsIn, cellsOut;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            std::string doc = "Column " + std::to_string(c + 1) + ", row " + std::to_string(r + 1) + ".";
            cellsIn.push_back(fin(cell(c, r), T::Float, c == r ? 1.0 : 0.0, Implicit::None, doc));
            cellsOut.push_back(fout(cell(c, r), T::Float, doc));
        }
    registerDecl({
        .type = "CombineMatrix",
        .label = "Combine Matrix",
        .category = "Utilities",
        .doc = "A 4x4 matrix from its sixteen values, column by column; the last column is the translation.",
        .sockets = {.inputs = cellsIn, .outputs = {fout("matrix", T::Matrix)}},
        .exec =
            [](ExecContext& c) {
                std::vector<FieldPtr> in;
                for (int k = 0; k < 16; ++k)
                    in.push_back(c.field(cell(k / 4, k % 4)));
                c.out("matrix", std::make_shared<FunctionField>(T::Matrix, in, std::vector<SocketType>(16, T::Float),
                                                                 [](const std::vector<Column>& cols, size_t n) {
                                                                     bool single = std::all_of(cols.begin(), cols.end(), [](const Column& x) { return x.single; });
                                                                     std::vector<M4> out(single ? 1 : n);
                                                                     for (size_t e = 0; e < out.size(); ++e)
                                                                         for (int k = 0; k < 16; ++k)
                                                                             out[e][k / 4][k % 4] = cols[size_t(k)].at<double>(e);
                                                                     return Column{std::move(out), single};
                                                                 }));
            },
    });
    registerDecl({
        .type = "SeparateMatrix",
        .label = "Separate Matrix",
        .category = "Utilities",
        .doc = "The sixteen values of a 4x4 matrix, column by column.",
        .sockets = {.inputs = {fin("matrix", T::Matrix)}, .outputs = cellsOut},
        .exec =
            [](ExecContext& c) {
                for (int k = 0; k < 16; ++k)
                    c.out(cell(k / 4, k % 4), map<double, M4>(T::Float, [k](const M4& m) { return m[k / 4][k % 4]; }, {c.field("matrix")}));
            },
    });
    registerDecl({
        .type = "CombineTransform",
        .label = "Combine Transform",
        .category = "Utilities",
        .doc = "A transform matrix from translation, rotation and scale: scaled first, then rotated, then moved.",
        .sockets = {.inputs = {fin("translation", T::Vector, vec(0, 0, 0)), fin("rotation", T::Rotation, vec(0, 0, 0)), fin("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {fout("transform", T::Matrix)}},
        .exec =
            [](ExecContext& c) {
                c.out("transform", map<M4, D3, D3, D3>(T::Matrix, [](D3 t, D3 r, D3 s) { return composeTransform(t, r, s); },
                                                       {c.field("translation"), c.field("rotation"), c.field("scale")}));
            },
    });
    registerDecl({
        .type = "SeparateTransform",
        .label = "Separate Transform",
        .category = "Utilities",
        .doc = "A transform matrix's translation, rotation and scale. Shear is lost; a mirrored matrix gives a negative X scale.",
        .sockets = {.inputs = {fin("transform", T::Matrix)},
                    .outputs = {fout("translation", T::Vector), fout("rotation", T::Rotation), fout("scale", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr m = c.field("transform");
                c.out("translation", map<D3, M4>(T::Vector, translationOf, {m}));
                c.out("rotation", map<D3, M4>(T::Rotation, rotationOf, {m}));
                c.out("scale", map<D3, M4>(T::Vector, scaleOf, {m}));
            },
    });
}

void mathNodes()
{
    registerDecl({
        .type = "MultiplyMatrices",
        .label = "Multiply Matrices",
        .category = "Utilities",
        .doc = "A times B: the transform that applies B first, then A.",
        .sockets = {.inputs = {fin("a", T::Matrix), fin("b", T::Matrix)}, .outputs = {fout("matrix", T::Matrix)}},
        .exec = [](ExecContext& c) { c.out("matrix", map<M4, M4, M4>(T::Matrix, [](const M4& a, const M4& b) { return a * b; }, {c.field("a"), c.field("b")})); },
    });
    registerDecl({
        .type = "InvertMatrix",
        .label = "Invert Matrix",
        .category = "Utilities",
        .doc = "The transform that undoes this one. A matrix that can't be inverted (a zero scale) passes through, with Invertible false.",
        .sockets = {.inputs = {fin("matrix", T::Matrix)}, .outputs = {fout("matrix", T::Matrix), fout("invertible", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr m = c.field("matrix");
                c.out("matrix", map<M4, M4>(T::Matrix, [](const M4& x) { return std::abs(glm::determinant(x)) > 1e-300 ? glm::inverse(x) : x; }, {m}));
                c.out("invertible", map<uint8_t, M4>(T::Bool, [](const M4& x) { return uint8_t(std::abs(glm::determinant(x)) > 1e-300); }, {m}));
            },
    });
    registerDecl({
        .type = "TransposeMatrix",
        .label = "Transpose Matrix",
        .category = "Utilities",
        .doc = "Rows become columns. For a pure rotation this is its inverse.",
        .sockets = {.inputs = {fin("matrix", T::Matrix)}, .outputs = {fout("matrix", T::Matrix)}},
        .exec = [](ExecContext& c) { c.out("matrix", map<M4, M4>(T::Matrix, [](const M4& x) { return glm::transpose(x); }, {c.field("matrix")})); },
    });
    registerDecl({
        .type = "MatrixDeterminant",
        .label = "Matrix Determinant",
        .category = "Utilities",
        .doc = "How much the transform scales volume; negative when it mirrors.",
        .sockets = {.inputs = {fin("matrix", T::Matrix)}, .outputs = {fout("determinant", T::Float)}},
        .exec = [](ExecContext& c) { c.out("determinant", map<double, M4>(T::Float, [](const M4& x) { return glm::determinant(x); }, {c.field("matrix")})); },
    });
    registerDecl({
        .type = "ProjectPoint",
        .label = "Project Point",
        .category = "Utilities",
        .doc = "Transforms a point by a projection matrix and divides by w, as a camera projects into its view.",
        .sockets = {.inputs = {fin("vector", T::Vector, vec(0, 0, 0)), fin("transform", T::Matrix)}, .outputs = {fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("vector", map<D3, D3, M4>(T::Vector,
                                                [](D3 p, const M4& m) {
                                                    D4 h = m * D4(p, 1.0);
                                                    return std::abs(h.w) > 1e-300 ? D3(h) / h.w : D3(h);
                                                },
                                                {c.field("vector"), c.field("transform")}));
            },
    });
}

class InstanceMatrixField final : public Field
{
public:
    InstanceMatrixField() : Field(SocketType::Matrix) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Instances)
            return Column::of(M4(1.0));
        return Column::of(ctx.instances->transforms);
    }
};

/**
 * Adds a mode property to an existing node: "components" keeps it as it is, "matrix" swaps the component inputs for one
 * matrix input and runs matrixExec instead.
 */
void addMatrixMode(const char* type, SocketDecl matrixInput, std::vector<std::string> components, std::function<void(ExecContext&)> matrixExec)
{
    const NodeDecl* base = findDecl(type);
    if (!base)
        return;
    NodeDecl d = *base;
    d.props.insert(d.props.begin(), prop("mode", {"components", "matrix"}, "components",
                                         "components: separate translation, rotation and scale; matrix: one transform matrix."));
    Sockets plain = d.sockets;
    d.dynamicSockets = [plain, matrixInput, components](const Group&, const GNode& n) {
        auto it = n.values.find("mode");
        if (it == n.values.end() || !it->is_string() || it->get<std::string>() != "matrix")
            return plain;
        Sockets s;
        bool placed = false;
        for (const SocketDecl& in : plain.inputs)
        {
            if (std::find(components.begin(), components.end(), in.id) == components.end())
                s.inputs.push_back(in);
            else if (!placed)
            {
                s.inputs.push_back(matrixInput);
                placed = true;
            }
        }
        s.outputs = plain.outputs;
        return s;
    };
    std::function<void(ExecContext&)> plainExec = d.exec;
    d.exec = [plainExec, matrixExec](ExecContext& c) {
        if (c.prop("mode") == "matrix")
            matrixExec(c);
        else
            plainExec(c);
    };
    registerDecl(std::move(d));
}

void transformModes()
{
    addMatrixMode("TransformGeometry", in("transform", T::Matrix, nullptr, "The transform to apply."), {"translation", "rotation", "scale"},
                  [](ExecContext& c) {
                      M4 m = evaluateSingle<M4>(c.field("transform"));
                      c.out("geometry", makeGeometry(transformed(*c.geometry("geometry"), m)));
                  });
    addMatrixMode("TransformPoint", fin("transform", T::Matrix, nullptr, Implicit::None, "The transform to apply."), {"translation", "rotation", "scale"},
                  [](ExecContext& c) {
                      c.out("vector", map<D3, D3, M4>(T::Vector, [](D3 p, const M4& m) { return D3(m * D4(p, 1.0)); }, {c.field("vector"), c.field("transform")}));
                  });
    addMatrixMode("TransformDirection", fin("transform", T::Matrix, nullptr, Implicit::None, "The transform to apply; its translation is ignored."),
                  {"rotation", "scale"}, [](ExecContext& c) {
                      c.out("direction", map<D3, D3, M4>(T::Vector, [](D3 d, const M4& m) { return glm::dmat3(m) * d; },
                                                         {c.field("direction"), c.field("transform")}));
                  });
    addMatrixMode("SetInstanceTransform", fin("transform", T::Matrix, nullptr, Implicit::None, "Each instance's new transform."),
                  {"translation", "rotation", "scale"}, [](ExecContext& c) {
                      c.out("instances", editComponents(c.geometry("instances"), {.instances = [&](Instances& in) {
                                                                                      FieldContext ctx = FieldContext::forInstances(in);
                                                                                      std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                                      std::vector<M4> m = evaluateAll<M4>(c.field("transform"), ctx);
                                                                                      for (size_t k = 0; k < in.size(); ++k)
                                                                                          if (sel[k])
                                                                                              in.transforms[k] = m[k];
                                                                                  }}));
                  });
    registerDecl({
        .type = "InstanceTransform",
        .label = "Instance Transform",
        .category = "Instances",
        .doc = "Each instance's whole transform as a matrix.",
        .sockets = {.outputs = {fout("transform", T::Matrix)}},
        .exec = [](ExecContext& c) { c.out("transform", std::make_shared<InstanceMatrixField>()); },
    });
}
} // namespace lila::gn::nodes::matrixnodes

namespace lila::gn::nodes
{
void registerMatrixNodes()
{
    matrixnodes::buildNodes();
    matrixnodes::mathNodes();
    matrixnodes::transformModes();
}
} // namespace lila::gn::nodes
