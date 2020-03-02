// These files were initially authored by Pixar.
// In 2019, Foundry and Pixar agreed Foundry should maintain and curate
// these plug-ins, and they moved to
// https://github.com/TheFoundryVisionmongers/mariusdplugins
// under the same Modified Apache 2.0 license as the main USD library,
// as shown below.
//
// Copyright 2019 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//

#include "GeoData.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec2f.h"

#include "pxr/base/vt/value.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/xformCache.h"

#include <float.h>
using namespace std;
PXR_NAMESPACE_USING_DIRECTIVE

TF_DEFINE_ENV_SETTING(MARI_READ_FLOAT2_AS_UV, true,
        "Set to false to disable ability to read Float2 type as a UV set");

std::vector<std::string> GeoData::_requireGeomPathSubstring;
std::vector<std::string> GeoData::_ignoreGeomPathSubstring;

std::string GeoData::_requireGeomPathSubstringEnvVar = "PX_USDREADER_REQUIRE_GEOM_PATH_SUBSTR";
std::string GeoData::_ignoreGeomPathSubstringEnvVar = "PX_USDREADER_IGNORE_GEOM_PATH_SUBSTR";

//#define PRINT_DEBUG
//#define PRINT_ARRAYS

//------------------------------------------------------------------------------
// GeoData implementation
//------------------------------------------------------------------------------

bool GeoData::ReadFloat2AsUV()
{
    static const bool readFloat2AsUV =
        TfGetEnvSetting(MARI_READ_FLOAT2_AS_UV);
    return readFloat2AsUV;
}

GeoData::GeoData(UsdPrim const &prim,
                 std::string uvSet,  // requested uvSet. If empty string, it's a ptex thing.
                 std::vector<int> frames,
                 bool keepCentered,
                 UsdPrim const &model,
                 const MriGeoReaderHost& host,
                 std::vector<std::string>& log)
{
    // Init
    m_isSubdivMesh = false;
    m_subdivisionScheme = "";
    m_interpolateBoundary = 0;
    m_faceVaryingLinearInterpolation = 0;
    m_propagateCorner = 0;

    UsdGeomMesh mesh(prim);
    if (not mesh)
    {
        host.trace("[GeoData:%d] Invalid non-mesh prim %s (type %s)", __LINE__, prim.GetPath().GetText(), prim.GetTypeName().GetText());
        log.push_back("Invalid non-mesh prim " + std::string(prim.GetPath().GetText()) + " of type " + std::string(prim.GetTypeName().GetText()));
        return;
    }

    bool isTopologyVarying = mesh.GetFaceVertexIndicesAttr().GetNumTimeSamples() >= 1;

//#if defined(PRINT_DEBUG)
    host.trace("[ !! ] ---------------------------------------");
    host.trace("[ GeoData:%d] Reading MESH %s (type %s) (topology Varying %d)", __LINE__, prim.GetPath().GetText(), prim.GetTypeName().GetText(), isTopologyVarying);
//#endif
    // Read vertex/face indices
    {
        VtIntArray vertsIndicesArray;
        bool ok = isTopologyVarying ? mesh.GetFaceVertexIndicesAttr().Get(&vertsIndicesArray, UsdTimeCode::EarliestTime()) : mesh.GetFaceVertexIndicesAttr().Get(&vertsIndicesArray);
        if (!ok)
        {
            host.trace("[GeoData:%d]\tfailed getting face vertex indices on %s.", __LINE__, prim.GetPath().GetText());
            log.push_back("Failed getting faces on " + std::string(prim.GetPath().GetText()));
            return;// this is not optional!
        }
        m_vertexIndices = vector<int>(vertsIndicesArray.begin(), vertsIndicesArray.end());
    }

    // Read face counts
    {
        VtIntArray nvertsPerFaceArray;
        bool ok = isTopologyVarying ? mesh.GetFaceVertexCountsAttr().Get(&nvertsPerFaceArray, UsdTimeCode::EarliestTime()) : mesh.GetFaceVertexCountsAttr().Get(&nvertsPerFaceArray);
        if (!ok)
        {
            host.trace("[GeoData:%d]\tfailed getting face counts on %s", __LINE__, prim.GetPath().GetText());
            log.push_back("Failed getting faces on " + std::string(prim.GetPath().GetText()));
            return;// this is not optional!
        }
        m_faceCounts = vector<int>(nvertsPerFaceArray.begin(), nvertsPerFaceArray.end());
    }

    // Create face selection indices
    {
        m_faceSelectionIndices.reserve(m_faceCounts.size());
        for(int x = 0; x < m_faceCounts.size(); ++x)
        {
            m_faceSelectionIndices.push_back(x);
        }
    }

    if (uvSet.length() > 0)
    {
        // Get UV set primvar
        if (UsdGeomPrimvar uvPrimvar = mesh.GetPrimvar(TfToken(uvSet)))
        {
            SdfValueTypeName typeName      = uvPrimvar.GetTypeName();
            TfToken          interpolation = uvPrimvar.GetInterpolation();

            // Only consider vertex or face varying uvs
            if ((interpolation == UsdGeomTokens->vertex or interpolation == UsdGeomTokens->faceVarying)
                and (typeName == SdfValueTypeNames->TexCoord2fArray or (GeoData::ReadFloat2AsUV() and typeName == SdfValueTypeNames->Float2Array)))
            {
                VtVec2fArray values;
                VtIntArray indices;
                if (uvPrimvar.Get(&values, UsdTimeCode::EarliestTime()))
                {
                    bool ok = isTopologyVarying ? uvPrimvar.GetIndices(&indices, UsdTimeCode::EarliestTime()) : uvPrimvar.GetIndices(&indices);
                    if (ok)
                    {
                        // primvar is indexed: validate/process values and indices together
                        m_uvIndices = vector<int>(indices.begin(), indices.end());
                    }
                    else
                    {
                        // Our uvs are not indexed -> we need to fill in an ordered list of indices
                        m_uvIndices.reserve(m_vertexIndices.size());
                        for (unsigned int x = 0; x < m_vertexIndices.size(); ++x)
                        {
                            m_uvIndices.push_back(x);
                        }
                    }

                    // Read uvs
                    m_uvs.resize(values.size()*2);
                    for (int i = 0; i < values.size(); ++i)
                    {
                        m_uvs[i * 2    ] = values[i][0];
                        m_uvs[i * 2 + 1] = values[i][1];
                    }
                }
                else
                {
                    host.trace("[GeoData:%d]\tdiscarding because could not read uvs on '%s'",  __LINE__, prim.GetPath().GetText());
                    log.push_back("discarding because could not read uvs on " +  std::string(prim.GetPath().GetText()));
                    return;
                }
            }
            else
            {
                host.trace("[GeoData:%d]\tDiscarding because Vertex or Facevarying interpolation is not defined for the \"%s\" uv set on %s",
                           __LINE__, uvSet.c_str(), prim.GetPath().GetText());
                log.push_back("Discarding because Vertex or Facevarying interpolation is not defined for the " + uvSet + " uv set on " + std::string(prim.GetPath().GetText()));
                return;
            }
        }
        else
        {
            host.trace("[GeoData:%d]\tDiscarding invalid uv set %s on %s", __LINE__, uvSet.c_str(), prim.GetPath().GetText());
            log.push_back("Discarding invalid uv set " + uvSet + " on " + std::string(prim.GetPath().GetText()));
            return;
        }
    }

    // Read normals
    {
        VtVec3fArray normalsVt;
        bool ok = isTopologyVarying ? mesh.GetNormalsAttr().Get(&normalsVt, UsdTimeCode::EarliestTime()) : mesh.GetNormalsAttr().Get(&normalsVt);
        if (ok)
        {
            m_normals.resize(normalsVt.size() * 3);
            for(int i = 0; i < normalsVt.size(); ++i)
            {
                m_normals[i * 3    ] = normalsVt[i][0];
                m_normals[i * 3 + 1] = normalsVt[i][1];
                m_normals[i * 3 + 2] = normalsVt[i][2];
            }

            m_normalIndices.reserve(m_vertexIndices.size());
            for (unsigned int x = 0; x < m_vertexIndices.size(); ++x)
            {
                m_normalIndices.push_back(x);
            }
        }
    }

    // Load vertices and animation frames
    GfMatrix4d const IDENTITY(1);
    vector<float> points;
    for (unsigned int iFrame = 0; iFrame < frames.size(); ++iFrame) 
    {
        // Get frame sample corresponding to frame index
        unsigned int frameSample = frames[iFrame];
        double currentTime = double(frameSample);

        // Read points for this frame sample
        VtVec3fArray pointsVt;
        if (!mesh.GetPointsAttr().Get(&pointsVt, frameSample))
        {
            host.trace("[GeoData:%d]\tfailed getting vertices on %s.", __LINE__, prim.GetPath().GetName().c_str());
            log.push_back("Failed getting faces on " + prim.GetPath().GetName());
            return;// this is not optional!
        }
        
        points.resize(pointsVt.size() * 3);
        for(int i = 0; i < pointsVt.size(); ++i) 
        {
            points[i * 3    ] = pointsVt[i][0];
            points[i * 3 + 1] = pointsVt[i][1];
            points[i * 3 + 2] = pointsVt[i][2];
        }

        // Calculate transforms - if not identity, pre-transform all points in place
        UsdGeomXformCache xformCache(currentTime);
        GfMatrix4d fullXform = xformCache.GetLocalToWorldTransform(prim);

        if (keepCentered)
        {
            // ignore transforms up to the model level
            GfMatrix4d m = xformCache.GetLocalToWorldTransform(model);
            fullXform = fullXform * m.GetInverse();
        }
        if (fullXform != IDENTITY)
        {
            unsigned int psize = points.size();
            for (unsigned int iPoint = 0; iPoint < psize; iPoint += 3)
            {
                GfVec4d p(points[iPoint], points[iPoint + 1], points[iPoint + 2], 1.0);
                p = p * fullXform;
                points[iPoint    ] = p[0];
                points[iPoint + 1] = p[1];
                points[iPoint + 2] = p[2];
            }
        }

        // Insert transformed vertices in our map
        m_vertices[frameSample].resize(points.size());
        m_vertices[frameSample] = points;
    }

    // DEBUG
#if defined(PRINT_DEBUG)
    {
        host.trace("[GeoData:%d]\t\t Face counts %i", __LINE__, m_faceCounts.size());
    #if defined(PRINT_ARRAYS)
        for (unsigned int x = 0; x < m_faceCounts.size(); ++x)
        {
            host.trace("\t\t face count[%d] : %d", x, m_faceCounts[x]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t vertex indices %i", __LINE__, m_vertexIndices.size());
    #if defined(PRINT_ARRAYS)
        for (unsigned x = 0; x < m_vertexIndices.size(); ++x)
        {
            host.trace("\t\t vertex Index[%d] : %d", x, m_vertexIndices[x]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t vertex frame count %i", __LINE__, m_vertices.size());
        vector<float> vertices0 = m_vertices.begin()->second;
        host.trace("[GeoData:%d]\t\t vertex @ frame0 count %i", __LINE__, vertices0.size()/3);
    #if defined(PRINT_ARRAYS)
        for (unsigned x = 0; x < vertices0.size()/3; ++x)
        {
            host.trace("\t\t vertex[%d] : (%f, %f, %f)", x, vertices0[(x*3)+0], vertices0[(x*3)+1], vertices0[(x*3)+2]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t uvs count %i", __LINE__, m_uvs.size()/2);
    #if defined(PRINT_ARRAYS)
        for(int x = 0; x < m_uvs.size()/2; ++x)
        {
            host.trace("\t\t uv[%d] : (%f, %f)", x, m_uvs[(x*2)+0], m_uvs[(x*2)+1]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t uv indices %i", __LINE__, m_uvIndices.size());
    #if defined(PRINT_ARRAYS)
        for(int x = 0; x < m_uvIndices.size(); ++x)
        {
            host.trace("\t\t UV Index[%d] : %d", x, m_uvIndices[x]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t normals count %i", __LINE__, m_normals.size()/3);
    #if defined(PRINT_ARRAYS)
        for(int x = 0; x < m_normals.size()/3; ++x)
        {
            host.trace("\t\t normal[%d] : (%f, %f, %f)", x, m_normals[(x*3)+0], m_normals[(x*3)+1], m_normals[(x*3)+2]);
        }
    #endif

        host.trace("[GeoData:%d]\t\t normals indices %i", __LINE__, m_normalIndices.size());
    #if defined(PRINT_ARRAYS)
        for(int x = 0; x < m_normalIndices.size(); ++x)
        {
            host.trace("\t\t Normal Index[%d] : %d", x, m_normalIndices[x]);
        }
    #endif
    }
#endif

    // Read OpenSubdiv structures
    {
        VtIntArray creaseIndicesArray;
        if (mesh.GetCreaseIndicesAttr().Get(&creaseIndicesArray))
        {
            m_creaseIndices = vector<int>(creaseIndicesArray.begin(), creaseIndicesArray.end());
        }

        VtIntArray creaseLengthsArray;
        if (mesh.GetCreaseLengthsAttr().Get(&creaseLengthsArray))
        {
            m_creaseLengths = vector<int>(creaseLengthsArray.begin(), creaseLengthsArray.end());
        }

        VtFloatArray creaseSharpnessArray;
        if (mesh.GetCreaseSharpnessesAttr().Get(&creaseSharpnessArray))
        {
            m_creaseSharpness = vector<float>(creaseSharpnessArray.begin(), creaseSharpnessArray.end());
        }

        VtIntArray cornerIndicesArray;
        if (mesh.GetCornerIndicesAttr().Get(&cornerIndicesArray))
        {
            m_cornerIndices = vector<int>(cornerIndicesArray.begin(), cornerIndicesArray.end());
        }

        VtFloatArray cornerSharpnessArray;
        if (mesh.GetCornerSharpnessesAttr().Get(&cornerSharpnessArray))
        {
            m_cornerSharpness = vector<float>(cornerSharpnessArray.begin(), cornerSharpnessArray.end());
        }

        VtIntArray holeIndicesArray;
        if (mesh.GetHoleIndicesAttr().Get(&holeIndicesArray))
        {
            m_holeIndices = vector<int>(holeIndicesArray.begin(), holeIndicesArray.end());
        }

        m_isSubdivMesh = false;
        TfToken subdivisionScheme;
        if (mesh.GetSubdivisionSchemeAttr().Get(&subdivisionScheme))
        {
            if (subdivisionScheme == UsdGeomTokens->none)
            {
                // This mesh is not subdivideable
                m_isSubdivMesh = false;
            }
            else
            {
                m_isSubdivMesh = true;

                if (subdivisionScheme == UsdGeomTokens->catmullClark)
                {
                    m_subdivisionScheme = "catmullClark";
                }
                else if (subdivisionScheme == UsdGeomTokens->loop)
                {
                    m_subdivisionScheme = "loop";
                }
                else if (subdivisionScheme == UsdGeomTokens->bilinear)
                {
                    m_subdivisionScheme = "bilinear";
                }

                TfToken interpolateBoundary;
                if (mesh.GetInterpolateBoundaryAttr().Get(&interpolateBoundary))
                {
                    if (interpolateBoundary == UsdGeomTokens->none)
                    {
                        m_interpolateBoundary = 0;
                    }
                    else if (interpolateBoundary == UsdGeomTokens->edgeAndCorner)
                    {
                        m_interpolateBoundary = 1;
                    }
                    else if (interpolateBoundary == UsdGeomTokens->edgeOnly)
                    {
                        m_interpolateBoundary = 2;
                    }
                }

                TfToken faceVaryingLinearInterpolation;
                if (mesh.GetFaceVaryingLinearInterpolationAttr().Get(&faceVaryingLinearInterpolation))
                {
                    // See MriOpenSubdivDialog::faceVaryingBoundaryInterpolationFromInt for reference

                    if (faceVaryingLinearInterpolation == UsdGeomTokens->all)
                    {
                        m_faceVaryingLinearInterpolation = 0;
                    }
                    else if (faceVaryingLinearInterpolation == UsdGeomTokens->none)
                    {
                        m_faceVaryingLinearInterpolation = 2;
                    }
                    else if (faceVaryingLinearInterpolation == UsdGeomTokens->boundaries)
                    {
                        m_faceVaryingLinearInterpolation = 3;
                    }
                    else if (faceVaryingLinearInterpolation == UsdGeomTokens->cornersPlus1)
                    {
                        m_faceVaryingLinearInterpolation = 1;
                        m_propagateCorner = 0;
                    }
                    else if (faceVaryingLinearInterpolation == UsdGeomTokens->cornersPlus2)
                    {
                        m_faceVaryingLinearInterpolation = 1;
                        m_propagateCorner = 1;
                    }
                }
            }
        }
    }
}

GeoData::~GeoData()
{
    Reset();
}


// Print the internal status of the Geometric Data.
void GeoData::Log(const MriGeoReaderHost& host)
{
}

// Cast to bool. False if no good data is found.
GeoData::operator bool()
{
    return (m_vertices.size() > 0 && m_vertices.begin()->second.size()>0 && m_vertexIndices.size()>0);
}

// Static - Sanity test to see if the usd prim is something we can use.
bool GeoData::IsValidNode(UsdPrim const &prim)
{
    if (not prim.IsA<UsdGeomMesh>())
        return false;
    else
        return TestPath( prim.GetPath().GetText());
}



// Pre-scan the UsdStage to see what uv sets are included.
void GeoData::GetUvSets(UsdPrim const &prim, UVSet &retval)
{
    UsdGeomGprim   gprim(prim);
    if (not gprim)
        return;

    vector<UsdGeomPrimvar> primvars = gprim.GetPrimvars();
    TF_FOR_ALL(primvar, primvars) 
    {
        TfToken          name, interpolation;
        SdfValueTypeName typeName;
        int              elementSize;

        primvar->GetDeclarationInfo(&name, &typeName, 
                                     &interpolation, &elementSize);
        
        if (interpolation == UsdGeomTokens->vertex or
             interpolation == UsdGeomTokens->faceVarying)
        {
            string prefix = name.GetString().substr(0,2);
            string mapName("");
        
            if ((prefix == "v_" || prefix == "u_") and
                (typeName == SdfValueTypeNames->FloatArray))
            {
                mapName = name.GetString().substr(2);
            } else if (typeName == SdfValueTypeNames->TexCoord2fArray ||
               (GeoData::ReadFloat2AsUV() &&
                typeName == SdfValueTypeNames->Float2Array))
            {
                mapName = name.GetString();
            }
            if (mapName.length()) {
                UVSet::iterator it = retval.find(mapName);
                if (it == retval.end())
                    retval[mapName] = 1;
                else
                    retval[mapName] += 1;
            }
        }
    }
}

float* GeoData::GetVertices(int frameSample)
{
    if (m_vertices.size() > 0)
    {
        if (m_vertices.find(frameSample) != m_vertices.end())
        {
            return &(m_vertices[frameSample][0]);
        }
        else
        {
            // Could not find frame -> let's return frame 0
            return &(m_vertices.begin()->second[0]);
        }
    }

    // frame not found.
    return NULL;
}

void GeoData::Reset()
{
    m_vertexIndices.clear();
    m_faceCounts.clear();
    m_faceSelectionIndices.clear();

    m_vertices.clear();

    m_normalIndices.clear();
    m_normals.clear();

    m_uvIndices.clear();
    m_uvs.clear();

    m_creaseIndices.clear();
    m_creaseLengths.clear();
    m_creaseSharpness.clear();
    m_cornerIndices.clear();
    m_cornerSharpness.clear();
    m_holeIndices.clear();
}

bool GeoData::TestPath(string path)
{
    bool requiredSubstringFound = true;
    std::vector<std::string>::iterator i;
    for (i=_requireGeomPathSubstring.begin(); i!=_requireGeomPathSubstring.end(); ++i)
    {
        if (path.find(*i) != string::npos)
        {
            requiredSubstringFound = true;
            break;
        } else {
            requiredSubstringFound = false;
        }
    }

    if (requiredSubstringFound == false) 
    {
        return false;
    }

    for (i=_ignoreGeomPathSubstring.begin(); i!=_ignoreGeomPathSubstring.end(); ++i)
    {
        if (path.find(*i) != string::npos)
        {
            return false;
        }
    }
    return true;
}
    

void GeoData::InitializePathSubstringLists()
{
    char * ignoreEnv = getenv(_ignoreGeomPathSubstringEnvVar.c_str());
    char * requireEnv = getenv(_requireGeomPathSubstringEnvVar.c_str());
    
    if (ignoreEnv) 
    {
        _ignoreGeomPathSubstring.clear();
        _ignoreGeomPathSubstring = TfStringTokenize(ignoreEnv, ",");
    }

    if (requireEnv) 
    {
        _requireGeomPathSubstring.clear();
        _requireGeomPathSubstring = TfStringTokenize(requireEnv, ",");
    }
}

template <typename SOURCE, typename TYPE>
bool GeoData::CastVtValueAs(SOURCE &obj, TYPE &result)
{
    if ( obj.template CanCast<TYPE>() ) 
    {
        obj = obj.template Cast<TYPE>();
        result = obj.template Get<TYPE>();
        return true;
    }
    return false;
}

