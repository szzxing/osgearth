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

#include <osgEarthAnnotation/AnnotationEditing>

#include <osg/io_utils>

using namespace osgEarth::Annotation;
using namespace osgEarth::Symbology;

/**********************************************************************/
class DraggerCallback : public osgManipulator::DraggerCallback
{
public:
    DraggerCallback(LocalizedNode* node, LocalizedNodeEditor* editor):
      _node(node),
      _editor( editor )
      {
          _ellipsoid = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();
      }

      osg::Vec3d getLocation(const osg::Matrixd& matrix)
      {
          osg::Vec3d trans = matrix.getTrans();
          double lat, lon, height;
          _ellipsoid->convertXYZToLatLongHeight(trans.x(), trans.y(), trans.z(), lat, lon, height);
          return osg::Vec3d(osg::RadiansToDegrees(lon), osg::RadiansToDegrees(lat), height);
      }

      virtual bool receive(const osgManipulator::MotionCommand& command)
      {
          switch (command.getStage())
          {
          case osgManipulator::MotionCommand::START:
              {
                  // Save the current matrix
                  osg::Vec3d location = _node->getPosition();
                  double x, y, z;
                  _ellipsoid->convertLatLongHeightToXYZ(osg::DegreesToRadians(location.y()), osg::DegreesToRadians(location.x()), location.z(), x, y, z);
                  _startMotionMatrix = osg::Matrixd::translate(x, y, z);

                  // Get the LocalToWorld and WorldToLocal matrix for this node.
                  osg::NodePath nodePathToRoot;
                  _localToWorld = osg::Matrixd::identity();
                  _worldToLocal = osg::Matrixd::identity();

                  return true;
              }
          case osgManipulator::MotionCommand::MOVE:
              {
                  // Transform the command's motion matrix into local motion matrix.
                  osg::Matrix localMotionMatrix = _localToWorld * command.getWorldToLocal()
                      * command.getMotionMatrix()
                      * command.getLocalToWorld() * _worldToLocal;


                  osg::Matrixd newMatrix = localMotionMatrix * _startMotionMatrix;
                  osg::Vec3d location = getLocation( newMatrix );

                  _node->setPosition( location );                     
                  _editor->updateDraggers();

                  return true;
              }
          case osgManipulator::MotionCommand::FINISH:
              {
                  return true;
              }
          case osgManipulator::MotionCommand::NONE:
          default:
              return false;
          }
      }


      osg::ref_ptr<const osg::EllipsoidModel>            _ellipsoid;
      LocalizedNode* _node;
      LocalizedNodeEditor* _editor;

      osg::Matrix _startMotionMatrix;

      osg::Matrix _localToWorld;
      osg::Matrix _worldToLocal;
};

/**********************************************************************/
LocalizedNodeEditor::LocalizedNodeEditor(LocalizedNode* node):
_node( node )
{
    _dragger  = new IntersectingDragger;
    _dragger->setNode( _node->getMapNode() );    
    _dragger->setHandleEvents( true );
    _dragger->addDraggerCallback(new DraggerCallback(_node, this) );        
    _dragger->setupDefaultGeometry();    
    addChild(_dragger);

    updateDraggers();
}

LocalizedNodeEditor::~LocalizedNodeEditor()
{    
}

void
LocalizedNodeEditor::updateDraggers()
{
    const osg::EllipsoidModel* em = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();

    osg::Matrixd matrix;
    osg::Vec3d location = _node->getPosition();    
    em->computeLocalToWorldTransformFromLatLongHeight(osg::DegreesToRadians( location.y() ),  osg::DegreesToRadians(location.x()), location.z(), matrix);
    _dragger->setMatrix(matrix);        
}


/**********************************************************************/

class SetRadiusCallback : public osgManipulator::DraggerCallback
{
public:
    SetRadiusCallback(CircleNode* node, osg::MatrixTransform* dragger, CircleNodeEditor* editor):
      _node(node),
      _dragger( dragger ),
      _editor( editor )
      {
          _ellipsoid = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();
      }

      osg::Vec3d getLocation(const osg::Matrixd& matrix)
      {
          osg::Vec3d trans = matrix.getTrans();
          double lat, lon, height;
          _ellipsoid->convertXYZToLatLongHeight(trans.x(), trans.y(), trans.z(), lat, lon, height);
          return osg::Vec3d(osg::RadiansToDegrees(lon), osg::RadiansToDegrees(lat), height);
      }

      virtual bool receive(const osgManipulator::MotionCommand& command)
      {
          switch (command.getStage())
          {
          case osgManipulator::MotionCommand::START:
              {
                  // Save the current matrix
                  _startMotionMatrix = _dragger->getMatrix();                  
                  osg::NodePath nodePathToRoot;
                  osgManipulator::computeNodePathToRoot(*_dragger,nodePathToRoot);
                  _localToWorld = osg::computeLocalToWorld(nodePathToRoot);
                  _worldToLocal = osg::Matrix::inverse(_localToWorld);
                  return true;
              }
          case osgManipulator::MotionCommand::MOVE:
              {
                  // Transform the command's motion matrix into local motion matrix.
                  osg::Matrix localMotionMatrix = _localToWorld * command.getWorldToLocal()
                      * command.getMotionMatrix()
                      * command.getLocalToWorld() * _worldToLocal;


                  osg::Matrixd newMatrix = localMotionMatrix * _startMotionMatrix;
                  osg::Vec3d radiusLocation = getLocation( newMatrix );

                  //Figure out the distance between the center of the circle and this new location
                  osg::Vec3d center = _node->getPosition();
                  double distance = GeoMath::distance(osg::DegreesToRadians( center.y() ), osg::DegreesToRadians( center.x() ), 
                                                      osg::DegreesToRadians( radiusLocation.y() ), osg::DegreesToRadians( radiusLocation.x() ),
                                                      _ellipsoid->getRadiusEquator());
                  _node->setRadius( Linear(distance, Units::METERS ) );
                  //The position of the radius dragger has changed, so recompute the bearing
                  _editor->computeBearing();
                  return true;
              }
          case osgManipulator::MotionCommand::FINISH:
              {
                  return true;
              }
          case osgManipulator::MotionCommand::NONE:
          default:
              return false;
          }
      }


      osg::ref_ptr<const osg::EllipsoidModel>            _ellipsoid;
      CircleNode* _node;
      CircleNodeEditor* _editor;

      osg::MatrixTransform* _dragger;
      osg::Matrix _startMotionMatrix;

      osg::Matrix _localToWorld;
      osg::Matrix _worldToLocal;
};



CircleNodeEditor::CircleNodeEditor( CircleNode* node ):
LocalizedNodeEditor( node ),
_radiusDragger( 0 ),
_bearing( osg::DegreesToRadians( 90.0 ) )
{
    _radiusDragger  = new IntersectingDragger;
    _radiusDragger->setNode( _node->getMapNode() );    
    _radiusDragger->setHandleEvents( true );
    _radiusDragger->addDraggerCallback(new SetRadiusCallback( node, _radiusDragger, this ) );        
    _radiusDragger->setColor(osg::Vec4(0,0,1,0));
    _radiusDragger->setupDefaultGeometry();    
    addChild(_radiusDragger);
    updateDraggers();
}

CircleNodeEditor::~CircleNodeEditor()
{
}

void
CircleNodeEditor::computeBearing()
{
    _bearing = osg::DegreesToRadians( 90.0 );
    //Get the radius dragger's position
    if (!_radiusDragger->getMatrix().isIdentity())
    {
        //Get the current location of the center of the circle
        osg::Vec3d location = _node->getPosition();

        const osg::EllipsoidModel* em = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();

        osg::Vec3d radiusDraggerWorld = _radiusDragger->getMatrix().getTrans();
        osg::Vec3d radiusMapRad;
        double height;
        em->convertXYZToLatLongHeight(radiusDraggerWorld.x(), radiusDraggerWorld.y(), radiusDraggerWorld.z(), 
            radiusMapRad.y(), radiusMapRad.x(), height);                

        //Get the current bearing, this is what we're going to use for the direction to try to maintain the current bearing so the editor stays consistent
        _bearing = GeoMath::bearing( osg::DegreesToRadians( location.y() ), osg::DegreesToRadians( location.x() ),
                                     radiusMapRad.y(), radiusMapRad.x());                    
    }
}

void
CircleNodeEditor::updateDraggers()
{
    LocalizedNodeEditor::updateDraggers();
    if (_radiusDragger)
    {
        const osg::EllipsoidModel* em = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();
        
        //Get the current location of the center of the circle
        osg::Vec3d location = _node->getPosition();    
        
        //Get the radius of the circle in meters
        double r = static_cast<CircleNode*>(_node.get())->getRadius().as(Units::METERS);

        double lat, lon;
        GeoMath::destination(osg::DegreesToRadians( location.y() ), osg::DegreesToRadians( location.x() ), _bearing, r, lat, lon, em->getRadiusEquator());        
        osg::Matrixd matrix;
        em->computeLocalToWorldTransformFromLatLongHeight(lat, lon, 0, matrix);
        _radiusDragger->setMatrix(matrix); 
    }
}



/***************************************************************************************************/


class SetEllipseRadiusCallback : public osgManipulator::DraggerCallback
{
public:
    SetEllipseRadiusCallback(EllipseNode* node, osg::MatrixTransform* dragger, EllipseNodeEditor* editor, bool major):
      _node(node),
      _dragger( dragger ),
      _editor( editor ),
      _major( major )
      {
          _ellipsoid = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();
      }

      osg::Vec3d getLocation(const osg::Matrixd& matrix)
      {
          osg::Vec3d trans = matrix.getTrans();
          double lat, lon, height;
          _ellipsoid->convertXYZToLatLongHeight(trans.x(), trans.y(), trans.z(), lat, lon, height);
          return osg::Vec3d(osg::RadiansToDegrees(lon), osg::RadiansToDegrees(lat), height);
      }

      virtual bool receive(const osgManipulator::MotionCommand& command)
      {
          switch (command.getStage())
          {
          case osgManipulator::MotionCommand::START:
              {
                  // Save the current matrix
                  _startMotionMatrix = _dragger->getMatrix();                  
                  osg::NodePath nodePathToRoot;
                  osgManipulator::computeNodePathToRoot(*_dragger,nodePathToRoot);
                  _localToWorld = osg::computeLocalToWorld(nodePathToRoot);
                  _worldToLocal = osg::Matrix::inverse(_localToWorld);
                  return true;
              }
          case osgManipulator::MotionCommand::MOVE:
              {
                  // Transform the command's motion matrix into local motion matrix.
                  osg::Matrix localMotionMatrix = _localToWorld * command.getWorldToLocal()
                      * command.getMotionMatrix()
                      * command.getLocalToWorld() * _worldToLocal;


                  //Get the location of the radius
                  osg::Matrixd newMatrix = localMotionMatrix * _startMotionMatrix;
                  osg::Vec3d radiusLocation = getLocation( newMatrix );

                  //Figure out the distance between the center of the circle and this new location
                  osg::Vec3d center = _node->getPosition();
                  double distance = GeoMath::distance(osg::DegreesToRadians( center.y() ), osg::DegreesToRadians( center.x() ), 
                                                      osg::DegreesToRadians( radiusLocation.y() ), osg::DegreesToRadians( radiusLocation.x() ),
                                                      _ellipsoid->getRadiusEquator());

                  double bearing = GeoMath::bearing(osg::DegreesToRadians( center.y() ), osg::DegreesToRadians( center.x() ), 
                                                      osg::DegreesToRadians( radiusLocation.y() ), osg::DegreesToRadians( radiusLocation.x() ));
        

                  //Compute the new angular rotation based on how they moved the point
                  if (_major)
                  {
                      _node->setRotationAngle( Angular( -bearing, Units::RADIANS ) );
                      _node->setRadiusMajor( Linear(distance, Units::METERS ) );
                  }
                  else
                  {
                      _node->setRotationAngle( Angular( osg::PI_2 - bearing, Units::RADIANS ) );
                      _node->setRadiusMinor( Linear(distance, Units::METERS ) );
                  }
                  _editor->updateDraggers();

                  return true;
              }
          case osgManipulator::MotionCommand::FINISH:
              {
                  return true;
              }
          case osgManipulator::MotionCommand::NONE:
          default:
              return false;
          }
      }


      osg::ref_ptr<const osg::EllipsoidModel>            _ellipsoid;
      EllipseNode* _node;
      EllipseNodeEditor* _editor;

      osg::MatrixTransform* _dragger;
      osg::Matrix _startMotionMatrix;

      osg::Matrix _localToWorld;
      osg::Matrix _worldToLocal;
      bool _major;
};





EllipseNodeEditor::EllipseNodeEditor( EllipseNode* node ):
LocalizedNodeEditor( node ),
_minorDragger( 0 ),
_majorDragger( 0 )
{
    _minorDragger  = new IntersectingDragger;
    _minorDragger->setNode( _node->getMapNode() );    
    _minorDragger->setHandleEvents( true );
    _minorDragger->addDraggerCallback(new SetEllipseRadiusCallback( node, _minorDragger, this, false ) );        
    _minorDragger->setColor(osg::Vec4(0,0,1,0));
    _minorDragger->setupDefaultGeometry();    
    addChild(_minorDragger);

    _majorDragger  = new IntersectingDragger;
    _majorDragger->setNode( _node->getMapNode() );    
    _majorDragger->setHandleEvents( true );
    _majorDragger->addDraggerCallback(new SetEllipseRadiusCallback( node, _majorDragger, this, true ) );        
    _majorDragger->setColor(osg::Vec4(1,0,0,1));
    _majorDragger->setupDefaultGeometry();    
    addChild(_majorDragger);

    updateDraggers();
}

EllipseNodeEditor::~EllipseNodeEditor()
{
}

void
EllipseNodeEditor::updateDraggers()
{
    LocalizedNodeEditor::updateDraggers();
    if (_majorDragger && _minorDragger)
    {
        const osg::EllipsoidModel* em = _node->getMapNode()->getMap()->getProfile()->getSRS()->getEllipsoid();
        
        //Get the current location of the center of the circle
        osg::Vec3d location = _node->getPosition();    
        
        //Get the raddi of the ellipse in meters
        EllipseNode* ellipse = static_cast<EllipseNode*>(_node.get());
        double majorR = ellipse->getRadiusMajor().as(Units::METERS);
        double minorR = ellipse->getRadiusMinor().as(Units::METERS);

        double rotation = ellipse->getRotationAngle().as( Units::RADIANS );

        double lat, lon;
        GeoMath::destination(osg::DegreesToRadians( location.y() ), osg::DegreesToRadians( location.x() ), osg::PI_2 - rotation, minorR, lat, lon, em->getRadiusEquator());        
        osg::Matrixd matrix;
        em->computeLocalToWorldTransformFromLatLongHeight(lat, lon, 0, matrix);
        _minorDragger->setMatrix(matrix); 

        GeoMath::destination(osg::DegreesToRadians( location.y() ), osg::DegreesToRadians( location.x() ), -rotation, majorR, lat, lon, em->getRadiusEquator());                
        em->computeLocalToWorldTransformFromLatLongHeight(lat, lon, 0, matrix);
        _majorDragger->setMatrix(matrix); 
    }
}