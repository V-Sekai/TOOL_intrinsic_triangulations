#include "../deps/geometry-central/include/geometrycentral/surface/manifold_surface_mesh.h"
#include "../deps/geometry-central/include/geometrycentral/surface/meshio.h"
#include "../deps/geometry-central/include/geometrycentral/surface/signpost_intrinsic_triangulation.h"
#include "../deps/geometry-central/include/geometrycentral/surface/surface_centers.h"
#include "../deps/geometry-central/include/geometrycentral/surface/vertex_position_geometry.h"

#include "../deps/polyscope/include/polyscope/point_cloud.h"
#include "../deps/polyscope/include/polyscope/polyscope.h"
#include "../deps/polyscope/include/polyscope/surface_mesh.h"

#include "../deps/polyscope/deps/args/args/args.hxx"
#include "imgui.h"

#include <sstream>

using namespace geometrycentral;
using namespace geometrycentral::surface;

// == Geometry-central data
std::unique_ptr<ManifoldSurfaceMesh> mesh;
std::unique_ptr<VertexPositionGeometry> geometry;
std::unique_ptr<CornerData<Vector2>> uvs;

std::unique_ptr<SignpostIntrinsicTriangulation> signpostTri;

// Polyscope visualization handle, to quickly add data to the surface
bool withGUI = true;
polyscope::SurfaceMesh* psMesh;

// Parameters
float refineDegreeThresh = 50;
float refineToSize = 20;
bool useRefineSizeThresh = true;
bool useInsertionsMax;
int insertionsMax = -2;

// Mesh stats
bool signpostIsDelaunay = true;
float signpostMinAngleDeg = 0.;

// Output options
std::string outputPrefix;

void updateTriagulationViz() {
  if (!withGUI) {
    return;
  }

  // Update stats
  signpostIsDelaunay = signpostTri->isDelaunay();
  signpostMinAngleDeg = signpostTri->minAngleDegrees();


  // Get the edge traces
  EdgeData<std::vector<SurfacePoint>> traces = signpostTri->traceAllIntrinsicEdgesAlongInput();

  // Convert to 3D positions
  std::vector<std::vector<Vector3>> traces3D(traces.size());
  size_t i = 0;
  for (Edge e : signpostTri->mesh.edges()) {
    for (SurfacePoint& p : traces[e]) {
      traces3D[i].push_back(p.interpolate(geometry->inputVertexPositions));
    }
    i++;
  }

  // Register with polyscope
  auto graphQ = polyscope::getSurfaceMesh()->addSurfaceGraphQuantity("intrinsic edges", traces3D);
  graphQ->setEnabled(true);
}

void resetTriangulation() {
  signpostTri.reset(new SignpostIntrinsicTriangulation(*mesh, *geometry));
  updateTriagulationViz();
}


void flipDelaunayTriangulation() {
  std::cout << "Flipping triangulation to Delaunay" << std::endl;
  signpostTri->flipToDelaunay();

  if (!signpostTri->isDelaunay()) {
    polyscope::warning("woah, failed to make mesh Delaunay with flips");
  }

  updateTriagulationViz();
}

void refineDelaunayTriangulation() {

  if (mesh->hasBoundary()) {
    polyscope::error("Support for refining meshes with boundary is experimental; proceed with caution!");
  }

  // Manage optional parameters
  double sizeParam = useRefineSizeThresh ? refineToSize : std::numeric_limits<double>::infinity();
  size_t maxInsertions = useInsertionsMax ? insertionsMax : INVALID_IND;

  std::cout << "Refining triangulation to Delaunay with:   degreeThresh=" << refineDegreeThresh
            << " circumradiusThresh=" << refineToSize << " maxInsertions=" << maxInsertions << std::endl;

  signpostTri->delaunayRefine(refineDegreeThresh, sizeParam, maxInsertions);

  if (!signpostTri->isDelaunay()) {
    polyscope::warning(
        "Failed to make mesh Delaunay with flips & refinement. Bug Nick to finish porting implementation.");
  }

  updateTriagulationViz();
}

template <typename T>
void saveMatrix(std::string filename, SparseMatrix<T>& matrix) {

  // WARNING: this follows matlab convention and thus is 1-indexed

  std::cout << "Writing sparse matrix to: " << filename << std::endl;

  std::ofstream outFile(filename);
  if (!outFile) {
    throw std::runtime_error("failed to open output file " + filename);
  }

  // Write a comment on the first line giving the dimensions
  outFile << "# sparse " << matrix.rows() << " " << matrix.cols() << std::endl;

  outFile << std::setprecision(16);

  for (int k = 0; k < matrix.outerSize(); ++k) {
    for (typename SparseMatrix<T>::InnerIterator it(matrix, k); it; ++it) {
      T val = it.value();
      size_t iRow = it.row();
      size_t iCol = it.col();

      outFile << (iRow + 1) << " " << (iCol + 1) << " " << val << std::endl;
    }
  }

  outFile.close();
}

template <typename T>
void saveMatrix(std::string filename, DenseMatrix<T>& matrix) {

  std::cout << "Writing dense matrix to: " << filename << std::endl;

  std::ofstream outFile(filename);
  if (!outFile) {
    throw std::runtime_error("failed to open output file " + filename);
  }

  // Write a comment on the first line giving the dimensions
  outFile << "# dense " << matrix.rows() << " " << matrix.cols() << std::endl;

  outFile << std::setprecision(16);

  for (size_t iRow = 0; iRow < (size_t)matrix.rows(); iRow++) {
    for (size_t iCol = 0; iCol < (size_t)matrix.cols(); iCol++) {
      T val = matrix(iRow, iCol);
      outFile << val;
      if (iCol + 1 != (size_t)matrix.cols()) {
        outFile << " ";
      }
    }
    outFile << std::endl;
  }

  outFile.close();
}

void outputIntrinsicFaces() {

  signpostTri->requireVertexIndices();

  // Assemble Fx3 adjacency matrices vertex indices and edge lengths
  size_t nV = signpostTri->mesh.nVertices();
  size_t nF = signpostTri->mesh.nFaces();

  DenseMatrix<double> faceLengths(nF, 3);
  DenseMatrix<size_t> faceInds(nF, 3);

  size_t iF = 0;
  for (Face f : signpostTri->mesh.faces()) {

    Halfedge he = f.halfedge();
    for (int v = 0; v < 3; v++) {

      Vertex vA = he.vertex();
      Vertex vB = he.twin().vertex();
      size_t indA = signpostTri->vertexIndices[vA];
      size_t indB = signpostTri->vertexIndices[vB];
      Edge e = he.edge();
      double l = signpostTri->inputEdgeLengths[e];

      faceLengths(iF, v) = l;
      faceInds(iF, v) = indA;

      he = he.next();
    }

    iF++;
  }

  saveMatrix("faceInds.dmat", faceInds);
  saveMatrix("faceLengths.dmat", faceLengths);
}

void outputVertexPositions() {

  signpostTri->requireVertexIndices();

  size_t nV = signpostTri->mesh.nVertices();
  DenseMatrix<double> vertexPositions(nV, 3);

  VertexData<Vector3> intrinsicPositions = signpostTri->sampleFromInput(geometry->inputVertexPositions);

  size_t iV = 0;
  for (Vertex v : signpostTri->mesh.vertices()) {
    Vector3 p = intrinsicPositions[v];
    vertexPositions(iV, 0) = p.x;
    vertexPositions(iV, 1) = p.y;
    vertexPositions(iV, 2) = p.z;
    iV++;
  }

  saveMatrix("vertexPositions.dmat", vertexPositions);
}

void outputVertexPositionsObj() {
  signpostTri->requireVertexIndices();
  signpostTri->requireFaceAreas();
  size_t nV = signpostTri->mesh.nVertices();
  DenseMatrix<double> vertexPositions(nV, 3);

  VertexData<Vector3> intrinsicPositions = signpostTri->sampleFromInput(geometry->inputVertexPositions);

  size_t iV = 0;
  for (Vertex v : signpostTri->mesh.vertices()) {
    Vector3 p = intrinsicPositions[v];
    vertexPositions(iV, 0) = p.x;
    vertexPositions(iV, 1) = p.y;
    vertexPositions(iV, 2) = p.z;
    iV++;
  }
  VertexPositionGeometry outputGeo(signpostTri->mesh, vertexPositions);
  writeSurfaceMesh(signpostTri->mesh, outputGeo, *uvs, "vertexPositions.obj");
}

void outputLaplaceMat() {
  signpostTri->requireCotanLaplacian();
  saveMatrix("laplace.spmat", signpostTri->cotanLaplacian);
}

void outputInterpolatMat() {
  signpostTri->requireVertexIndices();

  // Assemble Fx3 adjacency matrices vertex indices and edge lengths
  size_t nV = signpostTri->mesh.nVertices();

  SparseMatrix<double> interpMat(nV, nV);
  std::vector<Eigen::Triplet<double>> triplets;

  size_t iV = 0;
  for (Vertex v : signpostTri->mesh.vertices()) {
    SurfacePoint p = signpostTri->vertexLocations[v];
    p = p.inSomeFace();

    Face f = p.face;

    int j = 0;
    for (Vertex n : f.adjacentVertices()) {
      size_t jV = signpostTri->vertexIndices[n];
      double w = p.faceCoords[j];
      if (w > 0) {
        triplets.emplace_back(iV, jV, w);
      }
      j++;
    }
    iV++;
  }

  interpMat.setFromTriplets(triplets.begin(), triplets.end());
  interpMat.makeCompressed();

  saveMatrix("interpolate.spmat", interpMat);
}

void myCallback() {

  ImGui::PushItemWidth(100);

  ImGui::TextUnformatted("Intrinsic triangulation:");
  ImGui::Text("  nVertices = %lu  nFaces = %lu", signpostTri->mesh.nVertices(), signpostTri->mesh.nFaces());
  if (signpostIsDelaunay) {
    ImGui::Text("  is Delaunay: yes  min angle = %.2f degrees", signpostMinAngleDeg);
  } else {
    ImGui::Text("  is Delaunay: no   min angle = %.2f degrees", signpostMinAngleDeg);
  }

  if (ImGui::Button("reset triangulation")) {
    resetTriangulation();
  }

  if (ImGui::TreeNode("Delaunay flipping")) {
    if (ImGui::Button("flip to Delaunay")) {
      flipDelaunayTriangulation();
    }
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Delaunay refinement")) {
    ImGui::InputFloat("degree threshold", &refineDegreeThresh);

    ImGui::Checkbox("refine large triangles", &useRefineSizeThresh);
    if (useRefineSizeThresh) {
      ImGui::InputFloat("size threshold (circumradius)", &refineToSize);
    }

    ImGui::Checkbox("limit number of insertions", &useInsertionsMax);
    if (useInsertionsMax) {
      ImGui::InputInt("num insertions", &insertionsMax);
    }

    if (ImGui::Button("Delaunay refine")) {
      refineDelaunayTriangulation();
    }
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Output")) {

    if (ImGui::Button("intrinsic faces")) outputIntrinsicFaces();
    if (ImGui::Button("vertex positions")) outputVertexPositions();
    if (ImGui::Button("export vertex positions")) outputVertexPositionsObj();
    if (ImGui::Button("Laplace matrix")) outputLaplaceMat();
    if (ImGui::Button("interpolate matrix")) outputInterpolatMat();

    ImGui::TreePop();
  }

  ImGui::PopItemWidth();
}

int main(int argc, char** argv) {

  // Configure the argument parser
  // clang-format off
  args::ArgumentParser parser("A demo of Navigating Intrinsic Triangulations");
  args::HelpFlag help(parser, "help", "Display this help message", {'h', "help"});
  args::Positional<std::string> inputFilename(parser, "mesh", "A .obj or .ply mesh file.");

  args::Group triangulation(parser, "triangulation");
  args::Flag flipDelaunay(triangulation, "flipDelaunay", "Flip edges to make the mesh intrinsic Delaunay", {"flipDelaunay"});
  args::Flag refineDelaunay(triangulation, "refineDelaunay", "Refine and flip edges to make the mesh intrinsic Delaunay and satisfy angle/size bounds", {"refineDelaunay"});
  args::ValueFlag<double> refineAngle(triangulation, "refineAngle", "Minimum angle threshold (in degrees). Default: 25.", {"refineAngle"}, 15.);
  args::ValueFlag<double> refineSizeCircum(triangulation, "refineSizeCircum", "Maximum triangle size, set by specifying the circumradius. Default: inf", {"refineSizeCircum"}, 25);
  args::ValueFlag<int> refineMaxInsertions(triangulation, "refineMaxInsertions", 
      "Maximum number of insertions during refinement. Use 0 for no max, or negative values to scale by number of vertices. Default: 10 * nVerts", 
      {"refineMaxInsertions"}, -10);

  args::Group output(parser, "ouput");
  args::Flag noGUI(output, "noGUI", "exit after processing and do not open the GUI", {"noGUI"});
  args::ValueFlag<std::string> outputPrefixArg(output, "outputPrefix", "Prefix to prepend to all output file paths. Default: intrinsic_", {"outputPrefix"}, "intrinsic_");
  args::Flag intrinsicFaces(output, "edgeLengths", "write the face information for the intrinsic triangulation. name: 'faceInds.dmat, faceLengths.dmat'", {"intrinsicFaces"});
  args::Flag vertexPositions(output, "vertexPositions", "write the vertex positions for the intrinsic triangulation. name: 'vertexPositions.dmat'", {"vertexPositions"});
  args::Flag exportObj(output, "obj", "write the vertex positions for the intrinsic triangulation. name: 'vertexPositions.obj'", {"exportObj"});  
  args::Flag laplaceMat(output, "laplaceMat", "write the Laplace-Beltrami matrix for the triangulation. name: 'laplace.spmat'", {"laplaceMat"});
  args::Flag interpolateMat(output, "interpolateMat", "write the matrix which expresses data on the intrinsic vertices as a linear combination of the input vertices. name: 'interpolate.mat'", {"interpolateMat"});
  // clang-format on


  // Parse args
  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  // Make sure a mesh name was given
  if (!inputFilename) {
    std::cout << parser;
    return EXIT_FAILURE;
  }

  // Set options
  withGUI = !noGUI;

  refineDegreeThresh = args::get(refineAngle);
  refineToSize = args::get(refineSizeCircum);
  useRefineSizeThresh = refineToSize < std::numeric_limits<double>::infinity();
  insertionsMax = args::get(refineMaxInsertions);
  useInsertionsMax = insertionsMax != 0;
  outputPrefix = args::get(outputPrefixArg);

  // Load mesh
  std::tie(mesh, geometry, uvs) = readParameterizedManifoldSurfaceMesh(args::get(inputFilename));

  // Sale max insertions by number of vertices if needed
  if (insertionsMax < 0) {
    insertionsMax *= -mesh->nVertices();
  }

  if (withGUI) {

    // Initialize polyscope
    polyscope::init();

    // Set the callback function
    polyscope::state::userCallback = myCallback;

    // Register the mesh with polyscope
    psMesh = polyscope::registerSurfaceMesh(polyscope::guessNiceNameFromPath(args::get(inputFilename)),
                                            geometry->inputVertexPositions, mesh->getFaceVertexList(),
                                            polyscopePermutations(*mesh));


    // Set vertex tangent spaces
    geometry->requireVertexTangentBasis();
    VertexData<Vector3> vBasisX(*mesh);
    for (Vertex v : mesh->vertices()) {
      vBasisX[v] = geometry->vertexTangentBasis[v][0];
    }
    polyscope::getSurfaceMesh()->setVertexTangentBasisX(vBasisX);

    // Set face tangent spaces
    geometry->requireFaceTangentBasis();
    FaceData<Vector3> fBasisX(*mesh);
    for (Face f : mesh->faces()) {
      fBasisX[f] = geometry->faceTangentBasis[f][0];
    }
    polyscope::getSurfaceMesh()->setFaceTangentBasisX(fBasisX);

    // Nice defaults
    psMesh->setEdgeWidth(0.5);
  }


  // Initialize triangulation
  resetTriangulation();

  // Perform any operations requested
  if (flipDelaunay) flipDelaunayTriangulation();
  if (refineDelaunay) refineDelaunayTriangulation();

  // Generate any outputs
  if (intrinsicFaces) outputIntrinsicFaces();
  if (vertexPositions) outputVertexPositions();
  if (laplaceMat) outputLaplaceMat();
  if (interpolateMat) outputInterpolatMat();
  if (exportObj) outputVertexPositionsObj();

  // Give control to the polyscope gui
  if (withGUI) {
    polyscope::show();
  }

  return EXIT_SUCCESS;
}
