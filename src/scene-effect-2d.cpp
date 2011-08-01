/*
 * Copyright © 2010-2011 Linaro Limited
 *
 * This file is part of the glmark2 OpenGL (ES) 2.0 benchmark.
 *
 * glmark2 is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * glmark2 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * glmark2.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *  Alexandros Frantzis (glmark2)
 */
#include <cmath>
#include <climits>
#include <numeric>

#include "scene.h"
#include "mat.h"
#include "stack.h"
#include "vec.h"
#include "log.h"
#include "program.h"
#include "shader-source.h"


SceneEffect2D::SceneEffect2D(Canvas &pCanvas) :
    Scene(pCanvas, "effect")
{
    mOptions["matrix"] = Scene::Option("matrix",
        "0,0,0;0,1,0;0,0,0",
        "The convolution matrix to use [format: \"a,b,c;d,e,f...\"");
    mOptions["normalize"] = Scene::Option("normalize", "true",
        "Whether to normalize the supplied convolution matrix [true,false]");
}

SceneEffect2D::~SceneEffect2D()
{
}

/*
 * Calculates the offset of the coefficient with index i
 * from the center of the kernel matrix. Note that we are
 * using the standard OpenGL texture coordinate system
 * (x grows rightwards, y grows upwards).
 */
static LibMatrix::vec2
calc_offset(unsigned int i, unsigned int width, unsigned int height)
{
    int x = i % width - (width - 1) / 2;
    int y = -(i / width - (height - 1) / 2);

    return LibMatrix::vec2(static_cast<float>(x),
                           static_cast<float>(y));

}

/**
 * Creates a fragment shader implementing 2D image convolution.
 *
 * In the mathematical definition of 2D convolution, the kernel/filter (2D
 * impulse response) is essentially mirrored in both directions (that is,
 * rotated 180 degrees) when being applied on a 2D block of data (eg pixels).
 *
 * Most image manipulation programs, however, use the term kernel/filter to
 * describe a 180 degree rotation of the 2D impulse response. This is more
 * intuitive from a human understanding perspective because this rotated matrix
 * can be regarded as a stencil that can be directly applied by just "placing"
 * it on the image.
 *
 * In order to be compatible with image manipulation programs, we will
 * use the same definition of kernel/filter (180 degree rotation of impulse
 * response). This also means that we don't need to perform the (implicit)
 * rotation of the kernel in our convolution implementation.
 *
 * @param array the array holding the filter coefficients in row-major
 *              order
 * @param width the width of the filter
 * @param width the height of the filter
 *
 * @return a string containing the frament source code
 */
static std::string
create_convolution_fragment_shader(std::vector<float> &array,
                                   unsigned int width, unsigned int height)
{
    static const std::string frg_shader_filename(GLMARK_DATA_PATH"/shaders/effect-2d-convolution.frag");
    ShaderSource source(frg_shader_filename);

    if (width * height != array.size()) {
        Log::error("Convolution filter size doesn't match supplied dimensions\n");
        return "";
    }

    /* Steps are needed to be able to access nearby pixels */
    source.add_const("TextureStepX", 1.0f/800.0f);
    source.add_const("TextureStepY", 1.0f/600.0f);

    std::stringstream ss_def;
    std::stringstream ss_convolution;

    /* Set up stringstream floating point options */
    ss_def << std::fixed;
    ss_convolution.precision(1);
    ss_convolution << std::fixed;
    
    ss_convolution << "result = ";

    for(std::vector<float>::const_iterator iter = array.begin();
        iter != array.end();
        iter++)
    {
        unsigned int i = iter - array.begin();

        /* Add Filter coefficient const definitions */
        ss_def << "const float Filter" << i << " = "
               << *iter << ";" << std::endl;

        /* Add convolution term using the current filter coefficient */
        LibMatrix::vec2 offset(calc_offset(i, width, height));
        ss_convolution << "texture2D(Texture0, TextureCoord + vec2("
                       << offset.x() << " * TextureStepX, "
                       << offset.y() << " * TextureStepY)) * Filter" << i;
        if (iter + 1 != array.end())
            ss_convolution << " +" << std::endl;
    }

    ss_convolution << ";" << std::endl;

    source.add(ss_def.str());
    source.replace("$CONVOLUTION$", ss_convolution.str());

    return source.str();
}

/** 
 * Splits a string using a delimiter
 * 
 * @param s the string to split
 * @param delim the delimitir to use
 * @param elems the string vector to populate
 */
static void
split(const std::string &s, char delim, std::vector<std::string> &elems)
{
    std::stringstream ss(s);

    std::string item;
    while(std::getline(ss, item, delim))
        elems.push_back(item);
}

/** 
 * Parses a string representation of a matrix and returns it
 * in row-major format.
 *
 * In the string representation, elements are delimited using
 * commas (',') and rows are delimited using semi-colons (';').
 * eg 0,0,0;0,1.0,0;0,0,0
 * 
 * @param str the matrix string representation to parse
 * @param filter the float vector to populate
 * @param[out] width the width of the matrix
 * @param[out] height the height of the matrix 
 * 
 * @return whether parsing succeeded
 */
static bool
parse_matrix(std::string &str, std::vector<float> &filter,
             unsigned int &width, unsigned int &height)
{
    std::vector<std::string> rows;
    unsigned int w = UINT_MAX;

    split(str, ';', rows);

    Log::debug("Parsing convolution matrix:\n");

    for (std::vector<std::string>::const_iterator iter = rows.begin();
         iter != rows.end();
         iter++)
    {
        std::vector<std::string> elems;
        split(*iter, ',', elems);

        if (w != UINT_MAX && elems.size() != w) {
            Log::error("Matrix row %u contains %u elements, whereas previous"
                       " rows had %u\n", 
                       iter - rows.begin(), elems.size(), w);
            return false;
        }

        w = elems.size();
        
        for (std::vector<std::string>::const_iterator iter_el = elems.begin();
             iter_el != elems.end();
             iter_el++)
        {
            std::stringstream ss(*iter_el);
            float f;

            ss >> f;
            filter.push_back(f);
            Log::debug("%f ", f);
        }

        Log::debug("\n");
    }

    width = w;
    height = rows.size();
    
    return true;
}

/** 
 * Normalizes a convolution filter.
 * 
 * @param filter the filter to normalize
 */
static void
normalize(std::vector<float> &filter)
{
    float sum = std::accumulate(filter.begin(), filter.end(), 0.0);

    /* 
     * If sum is essentially zero, perform a zero-sum normalization.
     * This normalizes positive and negative values separately,
     */
    if (fabs(sum) < 0.00000001) {
        sum = 0.0;
        for (std::vector<float>::iterator iter = filter.begin();
             iter != filter.end();
             iter++)
        {
            if (*iter > 0.0)
                sum += *iter;
        }
    }

    /* 
     * We can simply compare with 0.0f here, because we just care about
     * avoiding division-by-zero.
     */
    if (sum == 0.0)
        return;
        
    for (std::vector<float>::iterator iter = filter.begin();
         iter != filter.end();
         iter++)
    {
        *iter /= sum; 
    }

}

int SceneEffect2D::load()
{
    Texture::load(GLMARK_DATA_PATH"/textures/effect-2d.png", &texture_,
                  GL_NEAREST, GL_NEAREST, 0);
    mRunning = false;

    return 1;
}

void SceneEffect2D::unload()
{
    glDeleteTextures(1, &texture_);
}

void SceneEffect2D::setup()
{
    Scene::setup();

    static const std::string vtx_shader_filename(GLMARK_DATA_PATH"/shaders/effect-2d.vert");

    std::vector<float> filter;
    unsigned int filter_width;
    unsigned int filter_height;

    /* Parse the matrix from the options */
    if (!parse_matrix(mOptions["matrix"].value, filter,
                      filter_width, filter_height))
    {
        return;
    }

    /* Normalize the matrix if needed */
    if (mOptions["normalize"].value == "true")
        normalize(filter);

    /* Create and load the shaders */
    ShaderSource vtx_source(vtx_shader_filename);
    ShaderSource frg_source;
    frg_source.append(create_convolution_fragment_shader(filter,
                                                         filter_width,
                                                         filter_height));

    if (frg_source.str().empty())
        return;

    if (!Scene::load_shaders_from_strings(program_, vtx_source.str(),
                                          frg_source.str()))
    {
        return;
    }

    std::vector<int> vertex_format;
    vertex_format.push_back(3);
    mesh_.set_vertex_format(vertex_format);

    mesh_.make_grid(1, 1, 2.0, 2.0, 0.0);
    mesh_.build_vbo();

    std::vector<GLint> attrib_locations;
    attrib_locations.push_back(program_.getAttribIndex("position"));
    mesh_.set_attrib_locations(attrib_locations);

    program_.start();

    // Load texture sampler value
    program_.loadUniformScalar(0, "Texture0");

    mCurrentFrame = 0;
    mRunning = true;
    mStartTime = Scene::get_timestamp_us() / 1000000.0;
    mLastUpdateTime = mStartTime;
}

void SceneEffect2D::teardown()
{
    mesh_.reset();

    program_.stop();
    program_.release();

    Scene::teardown();
}

void SceneEffect2D::update()
{
    double current_time = Scene::get_timestamp_us() / 1000000.0;
    double elapsed_time = current_time - mStartTime;

    mLastUpdateTime = current_time;

    if (elapsed_time >= mDuration) {
        mAverageFPS = mCurrentFrame / elapsed_time;
        mRunning = false;
    }

    mCurrentFrame++;
}

void SceneEffect2D::draw()
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

    mesh_.render_vbo();
}

Scene::ValidationResult
SceneEffect2D::validate()
{
    return ValidationUnknown;
}
