#ifndef HEIGHTBRUSH_H
#define HEIGHTBRUSH_H
/*************************************************************************************************\
HeightBrush.h : Radius terrain-sculpt brush (raise / lower / smooth) with a visible
                magenta radius cursor, adjustable size + strength, one step per stroke
                (each vertex is moved at most once per drag so passes layer cleanly).
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

#ifndef ACTION_H
#include "Action.h"
#endif

#include <set>

class HeightBrush : public Brush
{
	public:

		enum Mode { RAISE = 0, LOWER = 1, SMOOTH = 2 };

		HeightBrush( Mode mode, float radius, float strength );
		virtual ~HeightBrush();

		virtual bool beginPaint();
		virtual Action* endPaint();
		virtual bool paint( glm::vec3& worldPos, int screenX, int screenY );
		virtual bool canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags ) { return true; }
		virtual void render( int screenX, int screenY );

		void  setRadius( float r )   { radius = r; }
		void  setStrength( float s ) { strength = s; }
		void  setMode( Mode m )      { mode = m; }
		float getRadius() const      { return radius; }
		float getStrength() const    { return strength; }
		Mode  getMode() const        { return mode; }

	private:

		Mode             mode;
		float            radius;     // world units
		float            strength;   // height delta (world units) at full falloff per pass
		ActionPaintTile* pCurAction;
		std::set<int>    touchedThisStroke;   // vertexNum set -> one step per stroke

		void applyToVertex( int row, int col, float worldDist );
		void invalidateQuadsAround( int row, int col );
};

#endif // HEIGHTBRUSH_H
