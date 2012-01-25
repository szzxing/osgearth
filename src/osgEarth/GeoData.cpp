/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2010 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/GeoData>
#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/Cube>

#include <osg/Notify>
#include <osg/Timer>

#include <gdal_priv.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>
#include <memory.h>

#include <sstream>
#include <iomanip>

#define LC "[GeoData] "

using namespace osgEarth;


namespace
{
    void s_normalizeLongitude( double& x )
    {
        while( x < -180. ) x += 360.;
        while( x >  180. ) x -= 360.;
    }

    bool s_crossesAntimeridian( double x0, double x1 )
    {
        return ((x0 < 0.0 && x1 > 0.0 && x0-x1 < -180.0) ||
                (x1 < 0.0 && x0 > 0.0 && x1-x0 < -180.0));
    }

    double s_westToEastLongitudeDistance( double west, double east )
    {
        return west < east ? east-west : fmod(east,360.)-west;
    }
}


Bounds::Bounds() :
osg::BoundingBoxImpl<osg::Vec3d>( DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX )
{
    //nop
}

Bounds::Bounds(double xmin, double ymin, double xmax, double ymax ) :
osg::BoundingBoxImpl<osg::Vec3d>( xmin, ymin, -DBL_MAX, xmax, ymax, DBL_MAX )
{
    //nop
}

bool
Bounds::isValid() const
{
    return xMin() <= xMax() && yMin() <= yMax();
}

bool 
Bounds::contains(double x, double y ) const
{
    return 
        isValid() &&
        x >= xMin() && x <= xMax() && y >= yMin() && y <= yMax();
}

bool
Bounds::contains(const Bounds& rhs) const
{
    return 
        isValid() && rhs.isValid() && 
        xMin() <= rhs.xMin() && xMax() >= rhs.xMax() &&
        yMin() <= rhs.yMin() && yMax() >= rhs.yMax();
}

void
Bounds::expandBy( double x, double y )
{
    osg::BoundingBoxImpl<osg::Vec3d>::expandBy( x, y, 0 );
}

void
Bounds::expandBy( double x, double y, double z )
{
    osg::BoundingBoxImpl<osg::Vec3d>::expandBy( x, y, z );
}

void
Bounds::expandBy( const Bounds& rhs )
{
    osg::BoundingBoxImpl<osg::Vec3d>::expandBy( rhs );
}

Bounds 
Bounds::unionWith(const Bounds& rhs) const
{
    if ( valid() && !rhs.valid() ) return *this;
    if ( !valid() && rhs.valid() ) return rhs;

    Bounds u;
    if ( intersects(rhs) ) {
        u.xMin() = xMin() >= rhs.xMin() && xMin() <= rhs.xMax() ? xMin() : rhs.xMin();
        u.xMax() = xMax() >= rhs.xMin() && xMax() <= rhs.xMax() ? xMax() : rhs.xMax();
        u.yMin() = yMin() >= rhs.yMin() && yMin() <= rhs.yMax() ? yMin() : rhs.yMin();
        u.yMax() = yMax() >= rhs.yMin() && yMax() <= rhs.yMax() ? yMax() : rhs.yMax();
        u.zMin() = zMin() >= rhs.zMin() && zMin() <= rhs.zMax() ? zMin() : rhs.zMin();
        u.zMax() = zMax() >= rhs.zMin() && zMax() <= rhs.zMax() ? zMax() : rhs.zMax();
    }
    return u;
}

Bounds
Bounds::intersectionWith(const Bounds& rhs) const 
{
    if ( valid() && !rhs.valid() ) return *this;
    if ( !valid() && rhs.valid() ) return rhs;

    if ( this->contains(rhs) ) return rhs;
    if ( rhs.contains(*this) ) return *this;

    if ( !intersects(rhs) ) return Bounds();

    double xmin, xmax, ymin, ymax;

    xmin = ( xMin() > rhs.xMin() && xMin() < rhs.xMax() ) ? xMin() : rhs.xMin();
    xmax = ( xMax() > rhs.xMin() && xMax() < rhs.xMax() ) ? xMax() : rhs.xMax();
    ymin = ( yMin() > rhs.yMin() && yMin() < rhs.yMax() ) ? yMin() : rhs.yMin();
    ymax = ( yMax() > rhs.yMin() && yMax() < rhs.yMax() ) ? yMax() : rhs.yMax();

    return Bounds(xmin, ymin, xmax, ymax);
}

double
Bounds::width() const {
    return xMax()-xMin();
}

double
Bounds::height() const {
    return yMax()-yMin();
}

double
Bounds::depth() const {
    return zMax()-zMin();
}

osg::Vec2d
Bounds::center2d() const {
    osg::Vec3d c = center();
    return osg::Vec2d( c.x(), c.y() );
}

double
Bounds::radius2d() const {
    return (center2d() - osg::Vec2d(xMin(),yMin())).length();
}

double
Bounds::area2d() const {
    return width() * height();
}

std::string
Bounds::toString() const {
    std::stringstream buf;
    buf << "(" << xMin() << "," << yMin() << " => " << xMax() << "," << yMax() << ")";
    std::string result;
    result = buf.str();
    return result;
}

void
Bounds::transform( const SpatialReference* from, const SpatialReference* to )
{
    from->transformExtent( to, _min.x(), _min.y(), _max.x(), _max.y() );
}

/*************************************************************/

GeoExtent GeoExtent::INVALID = GeoExtent();


GeoExtent::GeoExtent():
_west   ( DBL_MAX ),
_east   ( DBL_MAX ),
_south  ( DBL_MAX ),
_north  ( DBL_MAX )
{
    //NOP - invalid
}

GeoExtent::GeoExtent(const SpatialReference* srs) :
_srs    ( srs ),
_west   ( DBL_MAX ),
_east   ( DBL_MAX ),
_south  ( DBL_MAX ),
_north  ( DBL_MAX )
{
    //nop
}

GeoExtent::GeoExtent(const SpatialReference* srs,
                     double west, double south, double east, double north ) :
_srs    ( srs ),
_west   ( west ),
_east   ( east ),
_south  ( south ),
_north  ( north )
{
    if ( srs && srs->isGeographic() )
    {
        s_normalizeLongitude( _west );
        s_normalizeLongitude( _east );
    }
}


GeoExtent::GeoExtent( const SpatialReference* srs, const Bounds& bounds ) :
_srs    ( srs ),
_west   ( bounds.xMin() ),
_east   ( bounds.xMax() ),
_south  ( bounds.yMin() ),
_north  ( bounds.yMax() )
{
    if ( srs && srs->isGeographic() )
    {
        s_normalizeLongitude( _west );
        s_normalizeLongitude( _east );
    }
}

GeoExtent::GeoExtent( const GeoExtent& rhs ) :
_srs  ( rhs._srs ),
_east ( rhs._east ),
_west ( rhs._west ),
_south( rhs._south ),
_north( rhs._north )
{
    //NOP
}

bool
GeoExtent::operator == ( const GeoExtent& rhs ) const
{
    if ( !isValid() && !rhs.isValid() )
        return true;

    if ( !isValid() || !rhs.isValid() )
        return false;

    return
        west()  == rhs.west()  &&
        east()  == rhs.east()  &&
        south() == rhs.south() &&
        north() == rhs.north() &&
        _srs->isEquivalentTo( rhs._srs.get() );
}

bool
GeoExtent::operator != ( const GeoExtent& rhs ) const
{
    return !( *this == rhs );
}

bool
GeoExtent::isValid() const
{
    return 
        _srs.valid()       && 
        _east  != DBL_MAX  &&
        _west  != DBL_MAX  &&
        _north != DBL_MAX  &&
        _south != DBL_MAX;
}

double
GeoExtent::width() const
{
    return crossesAntimeridian() ?
        (180.0-_west) + (_east+180.0) :
        _east - _west;
}

double
GeoExtent::height() const
{
    return _north - _south;
}

bool
GeoExtent::getCentroid( double& out_x, double& out_y ) const
{
    if ( isInvalid() ) return false;

    out_y = south() + 0.5*height();
    out_x = west() + 0.5*width();
    s_normalizeLongitude( out_x );
    return true;
}

bool
GeoExtent::crossesAntimeridian() const
{
    return _srs.valid() && _srs->isGeographic() && east() < west();
}

bool
GeoExtent::splitAcrossAntimeridian( GeoExtent& out_west, GeoExtent& out_east ) const
{
    bool success = false;

    if ( crossesAntimeridian() )
    {
        out_west._srs   = _srs.get();
        out_west._west  = west();
        out_west._south = south();
        out_west._east  = 180.0;
        out_west._north = north();

        out_east._srs   = _srs.get();
        out_east._west  = -180.0;
        out_east._south = south();
        out_east._east  = east();
        out_east._north = north();

        success = true;
    }
    else if ( !_srs->isGeographic() )
    {
        //note: may not actually work.
        GeoExtent latlong_extent = transform( _srs->getGeographicSRS() );
        GeoExtent w, e;
        if ( latlong_extent.splitAcrossAntimeridian( w, e ) )
        {
            out_west = w.transform( _srs.get() );
            out_east = e.transform( _srs.get() );
            success = out_west.isValid() && out_east.isValid();
        }
    }
    return success;
}

GeoExtent
GeoExtent::transform( const SpatialReference* to_srs ) const 
{       
    //TODO: this probably doesn't work across the antimeridian
    if ( _srs.valid() && to_srs )
    {
        double xmin = west(), ymin = south();
        double xmax = east(), ymax = north();
        
        if ( _srs->transformExtent( to_srs, xmin, ymin, xmax, ymax ) )
        {
            return GeoExtent( to_srs, xmin, ymin, xmax, ymax );
        }

    }
    return GeoExtent::INVALID;
}

void
GeoExtent::getBounds(double &xmin, double &ymin, double &xmax, double &ymax) const
{
    xmin = west();
    ymin = south();
    xmax = east();
    ymax = north();
}

Bounds
GeoExtent::bounds() const
{
    return Bounds( _west, _south, _east, _north );
}

bool
GeoExtent::contains(double x, double y, const SpatialReference* srs) const
{
    double local_x = x, local_y = y;
    if (srs &&
        !srs->isEquivalentTo( _srs.get() ) &&
        !srs->transform2D(x, y, _srs.get(), local_x, local_y) )
    {
        return false;
    }
    else
    {
        // normalize a geographic longitude to -180:+180
        if ( _srs->isGeographic() )
            s_normalizeLongitude(x);

        //Account for small rounding errors along the edges of the extent
        if (osg::equivalent(_west, local_x)) local_x = _west;
        if (osg::equivalent(_east, local_x)) local_x = _east;
        if (osg::equivalent(_south, local_y)) local_y = _south;
        if (osg::equivalent(_north, local_y)) local_y = _north;

        if ( crossesAntimeridian() )
        {
            if ( local_x > 0.0 )
            {
                return local_x >= _west && local_x <= 180.0 && local_y >= _south && local_y <= _north;
            }
            else
            {
                return local_x >= -180.0 && local_x <= _east && local_y >= _south && local_y <= _north;
            }
        }
        else
        {
            return local_x >= _west && local_x <= _east && local_y >= _south && local_y <= _north;
        }
    }
}

bool
GeoExtent::contains( const Bounds& rhs ) const
{
    return
        isValid() &&
        rhs.isValid() &&
        contains( rhs.xMin(), rhs.yMin() ) &&
        contains( rhs.xMax(), rhs.yMax() ) &&
        contains( rhs.center() );
}

bool
GeoExtent::contains( const GeoExtent& rhs ) const
{
    return
        isValid() &&
        rhs.isValid() &&
        contains( rhs.west(), rhs.south() ) &&
        contains( rhs.east(), rhs.north() ) &&
        contains( rhs.getCentroid() );   // this accounts for the antimeridian
}

bool
GeoExtent::intersects( const GeoExtent& rhs ) const
{
    if ( !isValid() ) return false;

    if ( rhs.crossesAntimeridian() )
    {
        GeoExtent rhsWest, rhsEast;
        rhs.splitAcrossAntimeridian( rhsWest, rhsEast );
        return rhsWest.intersects(*this) || rhsEast.intersects(*this);
    }
    else if ( crossesAntimeridian() )
    {
        GeoExtent west, east;
        splitAcrossAntimeridian(west, east);
        return rhs.intersects(west) || rhs.intersects(east);
    }
    else
    {
        bool exclusive =
            _west >= rhs.east() ||
            _east <= rhs.west() ||
            _south >= rhs.north() ||
            _north <= rhs.south();

        return !exclusive;
    }
}

void
GeoExtent::expandToInclude( double x, double y )
{
    if ( west() == DBL_MAX )
    {
        _west = x;
        _east = x;
        _south = y;
        _north = y;
    }
    else if ( getSRS() && getSRS()->isGeographic() )
    {
        s_normalizeLongitude(x);

        // calculate possible expansion distances. The lesser of the two
        // will be the direction in which we expand.

        // west:
        double dw;
        if ( x > west() )
            dw = west() - (x-360.);
        else
            dw = west() - x;

        // east:
        double de;
        if ( x < east() )
            de = (x+360.) - east();
        else
            de = x - east();

        // this is the empty space available - growth beyond this 
        // automatically yields full extent [-180..180]
        double maxWidth = 360.0-width();

        // if both are > 180, then the point is already in our extent.
        if ( dw <= 180. || de <= 180. )
        {
            if ( dw < de )
            {
                if ( dw < maxWidth )
                {
                    // expand westward
                    _west -= dw;
                    s_normalizeLongitude( _west );
                }
                else
                {
                    // reached full extent
                    _west = -180.0;
                    _east =  180.0;
                }
            }
            else
            {
                if ( de < maxWidth )
                {
                    // expand eastward
                    _east += de;
                    s_normalizeLongitude(_east);
                }
                else
                {
                    // reached full extent.
                    _west = -180.0;
                    _east =  180.0;
                }
            }
        }
        //else already inside longitude extent
    }
    else
    {
        _west = std::min(_west, x);
        _east = std::max(_east, x);
    }

    _south = std::min(_south, y);
    _north = std::max(_north, y);
}

void
GeoExtent::expandToInclude(const Bounds& rhs)
{
    //expandToInclude( rhs.xMin(), rhs.yMin() );
    //expandToInclude( rhs.xMin(), rhs.yMax() );
    //expandToInclude( rhs.xMax(), rhs.yMin() );
    //expandToInclude( rhs.xMax(), rhs.yMax() );

    expandToInclude( rhs.center() );
    expandToInclude( rhs.xMin(), rhs.yMin() );
    expandToInclude( rhs.xMax(), rhs.yMax() );
}

#if 0

GeoExtent
GeoExtent::intersectionSameSRS( const Bounds& rhs ) const
{
    Bounds b(
        osg::maximum( xMin(), rhs.xMin() ),
        osg::maximum( yMin(), rhs.yMin() ),
        osg::minimum( xMax(), rhs.xMax() ),
        osg::minimum( yMax(), rhs.yMax() ) );

    return b.width() > 0 && b.height() > 0 ? GeoExtent( getSRS(), b ) : GeoExtent::INVALID;
}
#endif

bool
GeoExtent::expandToInclude( const GeoExtent& rhs )
{
    if ( isInvalid() || rhs.isInvalid() ) return false;

    if ( !rhs.getSRS()->isEquivalentTo( _srs.get() ) )
    {
        return expandToInclude( transform(rhs.getSRS()) );
    }
    else
    {
        // include the centroid first in order to honor an 
        // antimeridian-crossing profile
        expandToInclude( rhs.getCentroid() );
        expandToInclude( rhs.west(), rhs.south() );
        expandToInclude( rhs.east(), rhs.north() );
        return true;
    }
}

GeoExtent
GeoExtent::intersectionSameSRS( const GeoExtent& rhs ) const
{
    if ( isInvalid() || rhs.isInvalid() || !_srs->isEquivalentTo( rhs.getSRS() ) )
        return GeoExtent::INVALID;

    if ( !intersects(rhs) )
    {
        OE_DEBUG << "Extents " << toString() << " and " << rhs.toString() << " do not intersect."
            << std::endl;
        return GeoExtent::INVALID;
    }

    GeoExtent result( *this );

    // use spherical vectors to consolidate in longitude:
    //osg::Vec3d westVec, eastVec, rhsWestVec, rhsEastVec;
    //getSRS()->transformToECEF(osg::Vec3d(west(), 0., 0.), westVec);
    //westVec.normalize();
    //getSRS()->transformToECEF(osg::Vec3d(east(), 0., 0.), eastVec);
    //eastVec.normalize();
    //getSRS()->transformToECEF(osg::Vec3d(rhs.west(), 0., 0.), rhsWestVec);
    //rhsWestVec.normalize();
    //getSRS()->transformToECEF(osg::Vec3d(rhs.east(), 0., 0.), rhsEastVec);
    //rhsEastVec.normalize();

    //double widthAngle = osg::DegreesToRadians(width());

    double westAngle, eastAngle;
    
    // see if the rhs western boundary intersects our extent:
    westAngle = s_westToEastLongitudeDistance( west(), rhs.west() );
    eastAngle = s_westToEastLongitudeDistance( rhs.west(), east() );
    //westAngle = acos(westVec*rhsWestVec);
    //eastAngle = acos(rhsWestVec*eastVec);
    if ( westAngle < width() && eastAngle < width() ) // yes, so adjust the result eastward:
    {
        result._west += westAngle;
    }

    // now see if the rhs eastern boundary intersects out extent:
    westAngle = s_westToEastLongitudeDistance( west(), rhs.east() );
    eastAngle = s_westToEastLongitudeDistance( rhs.east(), east() );
    //westAngle = acos(westVec*rhsEastVec);
    //eastAngle = acos(rhsEastVec*eastVec);
    if ( westAngle < width() && eastAngle < width() ) // yes, so adjust again:
    {
        result._east -= eastAngle;
    }

    // normalize our new longitudes
    s_normalizeLongitude( result._west );
    s_normalizeLongitude( result._east );

    // latitude is easy, just clamp it
    result._south = std::max( south(), rhs.south() );
    result._north = std::min( north(), rhs.north() );

    OE_DEBUG << "Intersection of " << this->toString() << " and " << rhs.toString() << " is: " 
        << result.toString()
        << std::endl;

    return result;
}

void
GeoExtent::scale(double x_scale, double y_scale)
{
    double orig_width = width();
    double orig_height = height();

    double new_width  = orig_width  * x_scale;
    double new_height = orig_height * y_scale;

    double halfXDiff = (new_width - orig_width) / 2.0;
    double halfYDiff = (new_height - orig_height) /2.0;

    _west  -= halfXDiff;
    _east  += halfXDiff;
    _south -= halfYDiff;
    _north += halfYDiff;
}

void
GeoExtent::expand( double x, double y )
{
    _west  -= .5*x;
    _east  += .5*x;
    _south -= .5*y;
    _north += .5*y;
}

double
GeoExtent::area() const
{
    return width() * height();
}

std::string
GeoExtent::toString() const
{
    std::stringstream buf;
    if ( !isValid() )
        buf << "INVALID";
    else
        buf << "SW=" << west() << "," << south() << " NE=" << east() << "," << north();

    buf << ", SRS=" << _srs->getName();

    std::string bufStr;
    bufStr = buf.str();
    return bufStr;
}


/***************************************************************************/

DataExtent::DataExtent(const osgEarth::GeoExtent &extent, unsigned int minLevel,  unsigned int maxLevel):
GeoExtent(extent),
_minLevel(minLevel),
_maxLevel(maxLevel)
{
}

unsigned int
DataExtent::getMinLevel() const
{
    return _minLevel;
}

unsigned int
DataExtent::getMaxLevel() const
{
    return _maxLevel;
}


/***************************************************************************/

// static
GeoImage GeoImage::INVALID( 0L, GeoExtent::INVALID );

GeoImage::GeoImage() :
_image(0L),
_extent( GeoExtent::INVALID )
{
    //nop
}


GeoImage::GeoImage( osg::Image* image, const GeoExtent& extent ) :
_image(image),
_extent(extent)
{
    //NOP
}

osg::Image*
GeoImage::getImage() const {
    return _image.get();
}

const SpatialReference*
GeoImage::getSRS() const {
    return _extent.getSRS();
}

const GeoExtent&
GeoImage::getExtent() const {
    return _extent;
}

double
GeoImage::getUnitsPerPixel() const {
	double uppw = _extent.width() / (double)_image->s();
	double upph = _extent.height() / (double)_image->t();
	return (uppw + upph) / 2.0;
}

GeoImage
GeoImage::crop( const GeoExtent& extent, bool exact, unsigned int width, unsigned int height  ) const
{
    //Check for equivalence
    if ( extent.getSRS()->isEquivalentTo( getSRS() ) )
    {
        //If we want an exact crop or they want to specify the output size of the image, use GDAL
        if (exact || width != 0 || height != 0 )
        {
            OE_DEBUG << "[osgEarth::GeoImage::crop] Performing exact crop" << std::endl;

            //Suggest an output image size
            if (width == 0 || height == 0)
            {
                double xRes = getExtent().width() / (double)_image->s(); //(getExtent().xMax() - getExtent().xMin()) / (double)_image->s();
                double yRes = getExtent().height() / (double)_image->t(); //(getExtent().yMax() - getExtent().yMin()) / (double)_image->t();

                width =  osg::maximum(1u, (unsigned int)(extent.width() / xRes));
                height = osg::maximum(1u, (unsigned int)(extent.height() / yRes));
                //width =  osg::maximum(1u, (unsigned int)((extent.xMax() - extent.xMin()) / xRes));
                //height = osg::maximum(1u, (unsigned int)((extent.yMax() - extent.yMin()) / yRes));

                OE_DEBUG << "[osgEarth::GeoImage::crop] Computed output image size " << width << "x" << height << std::endl;
            }

            //Note:  Passing in the current SRS simply forces GDAL to not do any warping
            return reproject( getSRS(), &extent, width, height);
        }
        else
        {
            OE_DEBUG << "[osgEarth::GeoImage::crop] Performing non-exact crop " << std::endl;
            //If an exact crop is not desired, we can use the faster image cropping code that does no resampling.
            double destXMin = extent.xMin();
            double destYMin = extent.yMin();
            double destXMax = extent.xMax();
            double destYMax = extent.yMax();

            osg::Image* new_image = ImageUtils::cropImage(
                _image.get(),
                _extent.xMin(), _extent.yMin(), _extent.xMax(), _extent.yMax(),
                destXMin, destYMin, destXMax, destYMax );

            //The destination extents may be different than the input extents due to not being able to crop along pixel boundaries.
            return new_image?
                GeoImage( new_image, GeoExtent( getSRS(), destXMin, destYMin, destXMax, destYMax ) ) :
                GeoImage::INVALID;
        }
    }
    else
    {
        //TODO: just reproject the image before cropping
        OE_NOTICE << "[osgEarth::GeoImage::crop] Cropping extent does not have equivalent SpatialReference" << std::endl;
        return GeoImage::INVALID;
    }
}

GeoImage
GeoImage::addTransparentBorder(bool leftBorder, bool rightBorder, bool bottomBorder, bool topBorder)
{
    unsigned int buffer = 1;

    unsigned int newS = _image->s();
    if (leftBorder) newS += buffer;
    if (rightBorder) newS += buffer;

    unsigned int newT = _image->t();
    if (topBorder)    newT += buffer;
    if (bottomBorder) newT += buffer;

    osg::Image* newImage = new osg::Image;
    newImage->allocateImage(newS, newT, _image->r(), _image->getPixelFormat(), _image->getDataType(), _image->getPacking());
    newImage->setInternalTextureFormat(_image->getInternalTextureFormat());
    memset(newImage->data(), 0, newImage->getImageSizeInBytes());
    unsigned startC = leftBorder ? buffer : 0;
    unsigned startR = bottomBorder ? buffer : 0;
    ImageUtils::copyAsSubImage(_image.get(), newImage, startC, startR );

    //double upp = getUnitsPerPixel();
    double uppw = _extent.width() / (double)_image->s();
	double upph = _extent.height() / (double)_image->t();

    double xmin = leftBorder ? _extent.xMin() - buffer * uppw : _extent.xMin();
    double ymin = bottomBorder ? _extent.yMin() - buffer * upph : _extent.yMin();
    double xmax = rightBorder ? _extent.xMax() + buffer * uppw : _extent.xMax();
    double ymax = topBorder ? _extent.yMax() + buffer * upph : _extent.yMax();

    return GeoImage(newImage, GeoExtent(getSRS(), xmin, ymin, xmax, ymax));
}

static osg::Image*
createImageFromDataset(GDALDataset* ds)
{
    // called internally -- GDAL lock not required

    //Allocate the image
    osg::Image *image = new osg::Image;
    image->allocateImage(ds->GetRasterXSize(), ds->GetRasterYSize(), 1, GL_RGBA, GL_UNSIGNED_BYTE);

    ds->RasterIO(GF_Read, 0, 0, image->s(), image->t(), (void*)image->data(), image->s(), image->t(), GDT_Byte, 4, NULL, 4, 4 * image->s(), 1);
    ds->FlushCache();

    image->flipVertical();

    return image;
}

static GDALDataset*
createMemDS(int width, int height, double minX, double minY, double maxX, double maxY, const std::string &projection)
{
    //Get the MEM driver
    GDALDriver* memDriver = (GDALDriver*)GDALGetDriverByName("MEM");
    if (!memDriver)
    {
        OE_NOTICE << "[osgEarth::GeoData] Could not get MEM driver" << std::endl;
    }

    //Create the in memory dataset.
    GDALDataset* ds = memDriver->Create("", width, height, 4, GDT_Byte, 0);

    //Initialize the color interpretation
    ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);

    //Initialize the geotransform
    double geotransform[6];
    double x_units_per_pixel = (maxX - minX) / (double)width;
    double y_units_per_pixel = (maxY - minY) / (double)height;
    geotransform[0] = minX;
    geotransform[1] = x_units_per_pixel;
    geotransform[2] = 0;
    geotransform[3] = maxY;
    geotransform[4] = 0;
    geotransform[5] = -y_units_per_pixel;
    ds->SetGeoTransform(geotransform);
    ds->SetProjection(projection.c_str());

    return ds;
}

static GDALDataset*
createDataSetFromImage(const osg::Image* image, double minX, double minY, double maxX, double maxY, const std::string &projection)
{
    //Clone the incoming image
    osg::ref_ptr<osg::Image> clonedImage = new osg::Image(*image);

    //Flip the image
    clonedImage->flipVertical();  

    GDALDataset* srcDS = createMemDS(image->s(), image->t(), minX, minY, maxX, maxY, projection);

    //Write the image data into the memory dataset
    //If the image is already RGBA, just read all 4 bands in one call
    if (image->getPixelFormat() == GL_RGBA)
    {
        srcDS->RasterIO(GF_Write, 0, 0, clonedImage->s(), clonedImage->t(), (void*)clonedImage->data(), clonedImage->s(), clonedImage->t(), GDT_Byte, 4, NULL, 4, 4 * image->s(), 1);
    }
    else if (image->getPixelFormat() == GL_RGB)
    {    
        //OE_NOTICE << "[osgEarth::GeoData] Reprojecting RGB " << std::endl;
        //Read the read, green and blue bands
        srcDS->RasterIO(GF_Write, 0, 0, clonedImage->s(), clonedImage->t(), (void*)clonedImage->data(), clonedImage->s(), clonedImage->t(), GDT_Byte, 3, NULL, 3, 3 * image->s(), 1);

        //Initialize the alpha values to 255.
        unsigned char *alpha = new unsigned char[clonedImage->s() * clonedImage->t()];
        memset(alpha, 255, clonedImage->s() * clonedImage->t());

        GDALRasterBand* alphaBand = srcDS->GetRasterBand(4);
        alphaBand->RasterIO(GF_Write, 0, 0, clonedImage->s(), clonedImage->t(), alpha, clonedImage->s(),clonedImage->t(), GDT_Byte, 0, 0);

        delete[] alpha;
    }
    else
    {
        OE_WARN << LC << "createDataSetFromImage: unsupported pixel format " << std::hex << image->getPixelFormat() << std::endl;
    }
    srcDS->FlushCache();

    return srcDS;
}

static osg::Image*
reprojectImage(osg::Image* srcImage, const std::string srcWKT, double srcMinX, double srcMinY, double srcMaxX, double srcMaxY,
               const std::string destWKT, double destMinX, double destMinY, double destMaxX, double destMaxY,
               int width = 0, int height = 0)
{
    //OE_NOTICE << "Reprojecting..." << std::endl;
    GDAL_SCOPED_LOCK;
	osg::Timer_t start = osg::Timer::instance()->tick();

    //Create a dataset from the source image
    GDALDataset* srcDS = createDataSetFromImage(srcImage, srcMinX, srcMinY, srcMaxX, srcMaxY, srcWKT);

	OE_DEBUG << "Source image is " << srcImage->s() << "x" << srcImage->t() << std::endl;


    if (width == 0 || height == 0)
    {
        double outgeotransform[6];
        double extents[4];
        void* transformer = GDALCreateGenImgProjTransformer(srcDS, srcWKT.c_str(), NULL, destWKT.c_str(), 1, 0, 0);
        GDALSuggestedWarpOutput2(srcDS,
            GDALGenImgProjTransform, transformer,
            outgeotransform,
            &width,
            &height,
            extents,
            0);
        GDALDestroyGenImgProjTransformer(transformer);
    }
	OE_DEBUG << "Creating warped output of " << width <<"x" << height << std::endl;
   
    GDALDataset* destDS = createMemDS(width, height, destMinX, destMinY, destMaxX, destMaxY, destWKT);

    GDALReprojectImage(srcDS, NULL,
                       destDS, NULL,
                       //GDALResampleAlg::GRA_NearestNeighbour,
                       GRA_Bilinear,
                       0,0,0,0,0);                    

    osg::Image* result = createImageFromDataset(destDS);
    
    delete srcDS;
    delete destDS;  

	osg::Timer_t end = osg::Timer::instance()->tick();

	OE_DEBUG << "Reprojected image in " << osg::Timer::instance()->delta_m(start,end) << std::endl;

    return result;
}    

static osg::Image*
manualReproject(const osg::Image* image, const GeoExtent& src_extent, const GeoExtent& dest_extent,
                unsigned int width = 0, unsigned int height = 0)
{
    //TODO:  Compute the optimal destination size
    if (width == 0 || height == 0)
    {
        //If no width and height are specified, just use the minimum dimension for the image
        width = osg::minimum(image->s(), image->t());
        height = osg::minimum(image->s(), image->t());        
    }

    // need to know this in order to choose the right interpolation algorithm
    const bool isSrcContiguous = src_extent.getSRS()->isContiguous();

    osg::Image *result = new osg::Image();
    result->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    //Initialize the image to be completely transparent
    memset(result->data(), 0, result->getImageSizeInBytes());

    ImageUtils::PixelReader ra(result);
    const double dx = dest_extent.width() / (double)width;
    const double dy = dest_extent.height() / (double)height;

    // offset the sample points by 1/2 a pixel so we are sampling "pixel center".
    // (This is especially useful in the UnifiedCubeProfile since it nullifes the chances for
    // edge ambiguity.)

    unsigned int numPixels = width * height;

    // Start by creating a sample grid over the destination
    // extent. These will be the source coordinates. Then, reproject
    // the sample grid into the source coordinate system.
    double *srcPointsX = new double[numPixels * 2];
    double *srcPointsY = srcPointsX + numPixels;
    dest_extent.getSRS()->transformExtentPoints(
        src_extent.getSRS(),
        dest_extent.xMin() + .5 * dx, dest_extent.yMin() + .5 * dy,
        dest_extent.xMax() - .5 * dx, dest_extent.yMax() - .5 * dy,
        srcPointsX, srcPointsY, width, height, 0, true);

    // Next, go through the source-SRS sample grid, read the color at each point from the source image,
    // and write it to the corresponding pixel in the destination image.
    int pixel = 0;
    ImageUtils::PixelReader ia(image);
    double xfac = (image->s() - 1) / src_extent.width();
    double yfac = (image->t() - 1) / src_extent.height();
    for (unsigned int c = 0; c < width; ++c)
    {
        for (unsigned int r = 0; r < height; ++r)
        {   
            double src_x = srcPointsX[pixel];
            double src_y = srcPointsY[pixel];

            if ( src_x < src_extent.xMin() || src_x > src_extent.xMax() || src_y < src_extent.yMin() || src_y > src_extent.yMax() )
            {
                //If the sample point is outside of the bound of the source extent, increment the pixel and keep looping through.
                //OE_WARN << LC << "ERROR: sample point out of bounds: " << src_x << ", " << src_y << std::endl;
                pixel++;
                continue;
            }

            float px = (src_x - src_extent.xMin()) * xfac;
            float py = (src_y - src_extent.yMin()) * yfac;

            int px_i = osg::clampBetween( (int)osg::round(px), 0, image->s()-1 );
            int py_i = osg::clampBetween( (int)osg::round(py), 0, image->t()-1 );

            osg::Vec4 color(0,0,0,0);

            if ( ! isSrcContiguous ) // non-contiguous space- use nearest neighbot
            {
                color = ia(px_i, py_i);
            }

            else // contiguous space - use bilinear sampling
            {
                int rowMin = osg::maximum((int)floor(py), 0);
                int rowMax = osg::maximum(osg::minimum((int)ceil(py), (int)(image->t()-1)), 0);
                int colMin = osg::maximum((int)floor(px), 0);
                int colMax = osg::maximum(osg::minimum((int)ceil(px), (int)(image->s()-1)), 0);

                if (rowMin > rowMax) rowMin = rowMax;
                if (colMin > colMax) colMin = colMax;

                osg::Vec4 urColor = ia(colMax, rowMax);
                osg::Vec4 llColor = ia(colMin, rowMin);
                osg::Vec4 ulColor = ia(colMin, rowMax);
                osg::Vec4 lrColor = ia(colMax, rowMin);

                /*Average Interpolation*/
                /*double x_rem = px - (int)px;
                double y_rem = py - (int)py;

                double w00 = (1.0 - y_rem) * (1.0 - x_rem);
                double w01 = (1.0 - y_rem) * x_rem;
                double w10 = y_rem * (1.0 - x_rem);
                double w11 = y_rem * x_rem;
                double wsum = w00 + w01 + w10 + w11;
                wsum = 1.0/wsum;

                color.r() = (w00 * llColor.r() + w01 * lrColor.r() + w10 * ulColor.r() + w11 * urColor.r()) * wsum;
                color.g() = (w00 * llColor.g() + w01 * lrColor.g() + w10 * ulColor.g() + w11 * urColor.g()) * wsum;
                color.b() = (w00 * llColor.b() + w01 * lrColor.b() + w10 * ulColor.b() + w11 * urColor.b()) * wsum;
                color.a() = (w00 * llColor.a() + w01 * lrColor.a() + w10 * ulColor.a() + w11 * urColor.a()) * wsum;*/

                /*Nearest Neighbor Interpolation*/
                /*if (px_i >= 0 && px_i < image->s() &&
                py_i >= 0 && py_i < image->t())
                {
                //OE_NOTICE << "[osgEarth::GeoData] Sampling pixel " << px << "," << py << std::endl;
                color = ImageUtils::getColor(image, px_i, py_i);
                }
                else
                {
                OE_NOTICE << "[osgEarth::GeoData] Pixel out of range " << px_i << "," << py_i << "  image is " << image->s() << "x" << image->t() << std::endl;
                }*/

                /*Bilinear interpolation*/
                //Check for exact value
                if ((colMax == colMin) && (rowMax == rowMin))
                {
                    //OE_NOTICE << "[osgEarth::GeoData] Exact value" << std::endl;
                    color = ia(px_i, py_i);
                }
                else if (colMax == colMin)
                {
                    //OE_NOTICE << "[osgEarth::GeoData] Vertically" << std::endl;
                    //Linear interpolate vertically
                    for (unsigned int i = 0; i < 4; ++i)
                    {
                        color[i] = ((float)rowMax - py) * llColor[i] + (py - (float)rowMin) * ulColor[i];
                    }
                }
                else if (rowMax == rowMin)
                {
                    //OE_NOTICE << "[osgEarth::GeoData] Horizontally" << std::endl;
                    //Linear interpolate horizontally
                    for (unsigned int i = 0; i < 4; ++i)
                    {
                        color[i] = ((float)colMax - px) * llColor[i] + (px - (float)colMin) * lrColor[i];
                    }
                }
                else
                {
                    //OE_NOTICE << "[osgEarth::GeoData] Bilinear" << std::endl;
                    //Bilinear interpolate
                    float col1 = colMax - px, col2 = px - colMin;
                    float row1 = rowMax - py, row2 = py - rowMin;
                    for (unsigned int i = 0; i < 4; ++i)
                    {
                        float r1 = col1 * llColor[i] + col2 * lrColor[i];
                        float r2 = col1 * ulColor[i] + col2 * urColor[i];

                        //OE_INFO << "r1, r2 = " << r1 << " , " << r2 << std::endl;
                        color[i] = row1 * r1 + row2 * r2;
                    }
                }
            }
               
            unsigned char* rgba = const_cast<unsigned char*>(ra.data(c,r,0));

            rgba[0] = (unsigned char)(color.r() * 255);
            rgba[1] = (unsigned char)(color.g() * 255);
            rgba[2] = (unsigned char)(color.b() * 255);
            rgba[3] = (unsigned char)(color.a() * 255);

            pixel++;            
        }
    }

    delete[] srcPointsX;

    return result;
}



GeoImage
GeoImage::reproject(const SpatialReference* to_srs, const GeoExtent* to_extent, unsigned int width, unsigned int height) const
{  
    GeoExtent destExtent;
    if (to_extent)
    {
        destExtent = *to_extent;
    }
    else
    {
         destExtent = getExtent().transform(to_srs);    
    }

    osg::Image* resultImage = 0L;

    if ( getSRS()->isUserDefined() || to_srs->isUserDefined() ||
        ( getSRS()->isMercator() && to_srs->isGeographic() ) ||
        ( getSRS()->isGeographic() && to_srs->isMercator() ) )
    {
        // if either of the SRS is a custom projection, we have to do a manual reprojection since
        // GDAL will not recognize the SRS.
        resultImage = manualReproject(getImage(), getExtent(), *to_extent, width, height);
    }
    else
    {
        // otherwise use GDAL.
        resultImage = reprojectImage(getImage(),
            getSRS()->getWKT(),
            getExtent().xMin(), getExtent().yMin(), getExtent().xMax(), getExtent().yMax(),
            to_srs->getWKT(),
            destExtent.xMin(), destExtent.yMin(), destExtent.xMax(), destExtent.yMax(),
            width, height);
    }   
    return GeoImage(resultImage, destExtent);
}

osg::Image*
GeoImage::takeImage()
{
    return _image.release();
}


/***************************************************************************/

// static
GeoHeightField GeoHeightField::INVALID( 0L, GeoExtent::INVALID, 0L );

GeoHeightField::GeoHeightField() :
_heightField( 0L ),
_extent( GeoExtent::INVALID ),
_vsrs( 0L )
{
    //nop
}

GeoHeightField::GeoHeightField(osg::HeightField* heightField,
                               const GeoExtent& extent,
                               const VerticalSpatialReference* vsrs) :
_heightField( heightField ),
_extent( extent ),
_vsrs( vsrs )
{
    if ( _heightField )
    {
        double minx, miny, maxx, maxy;
        _extent.getBounds(minx, miny, maxx, maxy);

        _heightField->setOrigin( osg::Vec3d( minx, miny, 0.0 ) );
        _heightField->setXInterval( (maxx - minx)/(double)(_heightField->getNumColumns()-1) );
        _heightField->setYInterval( (maxy - miny)/(double)(_heightField->getNumRows()-1) );
        _heightField->setBorderWidth( 0 );
    }
}

bool
GeoHeightField::getElevation(const osgEarth::SpatialReference* inputSRS, 
                             double x, double y, 
                             ElevationInterpolation interp,
                             const VerticalSpatialReference* outputVSRS,
                             float &elevation) const
{
    double local_x = x, local_y = y;

    if ( inputSRS && !inputSRS->transform2D(x, y, _extent.getSRS(), local_x, local_y) )
        return false;

    if ( _extent.contains(local_x, local_y) )
    {
        double xInterval = _extent.width()  / (double)(_heightField->getNumColumns()-1);
        double yInterval = _extent.height() / (double)(_heightField->getNumRows()-1);

        elevation = HeightFieldUtils::getHeightAtLocation(
            _heightField.get(), 
            local_x, local_y, 
            _extent.xMin(), _extent.yMin(), 
            xInterval, yInterval, 
            interp);

        if ( elevation != NO_DATA_VALUE )
        {
            if ( VerticalSpatialReference::canTransform( _vsrs.get(), outputVSRS ) )
            {
                // need geodetic coordinates for a VSRS transformation:
                double lat_deg, lon_deg, newElevation;

                if ( inputSRS->isGeographic() ) {
                    lat_deg = y;
                    lon_deg = x;
                }
                else if ( _extent.getSRS()->isGeographic() ) {
                    lat_deg = local_y;
                    lon_deg = local_x;
                }
                else {
                    _extent.getSRS()->transform2D( x, y, inputSRS->getGeographicSRS(), lon_deg, lat_deg );
                }

                if ( _vsrs->transform( outputVSRS, lat_deg, lon_deg, elevation, newElevation ) )
                    elevation = newElevation;
            }
        }

        return true;
    }
    else
    {
        elevation = 0.0f;
        return false;
    }
}

GeoHeightField
GeoHeightField::createSubSample( const GeoExtent& destEx, ElevationInterpolation interpolation) const
{
    double div = destEx.width()/_extent.width();
    if ( div >= 1.0f )
        return GeoHeightField::INVALID;

    int w = _heightField->getNumColumns();
    int h = _heightField->getNumRows();
    //double dx = _heightField->getXInterval() * div;
    //double dy = _heightField->getYInterval() * div;
    double xInterval = _extent.width() / (double)(_heightField->getNumColumns()-1);
    double yInterval = _extent.height() / (double)(_heightField->getNumRows()-1);
    double dx = xInterval * div;
    double dy = yInterval * div;

    osg::HeightField* dest = new osg::HeightField();
    dest->allocate( w, h );
    dest->setXInterval( dx );
    dest->setYInterval( dy );

    // copy over the skirt height, adjusting it for tile size.
    dest->setSkirtHeight( _heightField->getSkirtHeight() * div );

    double x, y;
    int col, row;

    for( x = destEx.xMin(), col=0; col < w; x += dx, col++ )
    {
        for( y = destEx.yMin(), row=0; row < h; y += dy, row++ )
        {
            float height = HeightFieldUtils::getHeightAtLocation( _heightField.get(), x, y, _extent.xMin(), _extent.yMin(), xInterval, yInterval, interpolation);
            dest->setHeight( col, row, height );
        }
    }

    osg::Vec3d orig( destEx.xMin(), destEx.yMin(), _heightField->getOrigin().z() );
    dest->setOrigin( orig );

    return GeoHeightField( dest, destEx, _vsrs.get() );
}

const GeoExtent&
GeoHeightField::getExtent() const
{
    return _extent;
}

const osg::HeightField*
GeoHeightField::getHeightField() const
{
    return _heightField.get();
}

osg::HeightField*
GeoHeightField::getHeightField() 
{
    return _heightField.get();
}

osg::HeightField*
GeoHeightField::takeHeightField()
{
    return _heightField.release();
}

// --------------------------------------------------------------------------

#undef  LC
#define LC "[Geoid] "

Geoid::Geoid() :
_hf( GeoHeightField::INVALID ),
_units( Units::METERS ),
_valid( false )
{
    //nop
}

void
Geoid::setName( const std::string& name )
{
    _name = name;
    validate();
}

void
Geoid::setHeightField( const GeoHeightField& hf )
{
    _hf = hf;
    validate();
}

void
Geoid::setUnits( const Units& units ) 
{
    _units = units;
    validate();
}

void
Geoid::validate()
{
    _valid = false;
    if ( !_hf.valid() ) {
        //OE_WARN << LC << "ILLEGAL GEOID: no heightfield" << std::endl;
    }
    else if ( !_hf.getExtent().getSRS() || !_hf.getExtent().getSRS()->isGeographic() ) {
        OE_WARN << LC << "ILLEGAL GEOID: heightfield must be geodetic" << std::endl;
    }
    else {
        _valid = true;
    }
}

float 
Geoid::getOffset(double lat_deg, double lon_deg, const ElevationInterpolation& interp ) const
{
    float result = 0.0f;

    if ( _valid )
    {
        // first convert the query coordinates to the geoid heightfield range if neccesary.
        if ( lat_deg < _hf.getExtent().yMin() )
            lat_deg = 90.0 - (-90.0-lat_deg);
        else if ( lat_deg > _hf.getExtent().yMax() )
            lat_deg = -90 + (lat_deg-90.0);
        if ( lon_deg < _hf.getExtent().xMin() )
            lon_deg += 360.0;
        else if ( lon_deg > _hf.getExtent().xMax() )
            lon_deg -= 360.0;

        bool ok = _hf.getElevation( 0L, lon_deg, lat_deg, interp, 0L, result );
        if ( !ok )
            result = 0.0f;
    }

    return result;
}

bool
Geoid::isEquivalentTo( const Geoid& rhs ) const
{
    // weak..
    return
        _valid &&
        _name == rhs._name &&
        _hf.getExtent() == rhs._hf.getExtent() &&
        _units == rhs._units;
}
