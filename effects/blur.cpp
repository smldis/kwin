/*****************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2007 Rivo Laks <rivolaks@hot.ee>

You can Freely distribute this program under the GNU General Public
License. See the file "COPYING" for the exact licensing terms.
******************************************************************/


#include "blur.h"

#include <kwinglutils.h>

#include <QString>
#include <KStandardDirs>

#include <kdebug.h>


namespace KWin
{

KWIN_EFFECT( Blur, BlurEffect );
KWIN_EFFECT_SUPPORTED( Blur, BlurEffect::supported() );


BlurEffect::BlurEffect() : Effect()
    {
    mSceneTexture = 0;
    mTmpTexture = 0;
    mBlurTexture = 0;
    mSceneTarget = 0;
    mTmpTarget = 0;
    mBlurTarget = 0;
    mBlurShader = 0;
    mWindowShader = 0;

    mBlurRadius = 4;
    mTime = 0;
    mValid = loadData();
    }

BlurEffect::~BlurEffect()
{
    delete mSceneTexture;
    delete mTmpTexture;
    delete mBlurTexture;
    delete mSceneTarget;
    delete mTmpTarget;
    delete mBlurTarget;
    delete mBlurShader;
    delete mWindowShader;
}


bool BlurEffect::loadData()
    {
    // Create texture and render target
    mSceneTexture = new GLTexture(displayWidth(), displayHeight());
    mSceneTexture->setFilter(GL_LINEAR);
    mTmpTexture = new GLTexture(displayWidth(), displayHeight());
    mTmpTexture->setFilter(GL_LINEAR);
    mBlurTexture = new GLTexture(displayWidth(), displayHeight());

    mSceneTarget = new GLRenderTarget(mSceneTexture);
    if( !mSceneTarget->valid() )
        return false;
    mTmpTarget = new GLRenderTarget(mTmpTexture);
    if( !mTmpTarget->valid() )
        return false;
    mBlurTarget = new GLRenderTarget(mBlurTexture);
    if( !mBlurTarget->valid() )
        return false;

    mBlurShader = loadShader("blur");
    if( !mBlurShader )
        return false;
    mWindowShader = loadShader("blur-render");
    if( !mWindowShader )
        return false;

    mBlurShader->bind();
    mBlurShader->setUniform("inputTex", 0);
    mBlurShader->setUniform("displayWidth", (float)displayWidth());
    mBlurShader->setUniform("displayHeight", (float)displayHeight());
    mBlurShader->unbind();

    mWindowShader->bind();
    mWindowShader->setUniform("windowTex", 0);
    mWindowShader->setUniform("backgroundTex", 4);
    mWindowShader->setUniform("displayWidth", (float)displayWidth());
    mWindowShader->setUniform("displayHeight", (float)displayHeight());
    mWindowShader->unbind();

    return true;
    }

GLShader* BlurEffect::loadShader(const QString& name)
{
    QString fragmentshader =  KGlobal::dirs()->findResource("data", "kwin/" + name + ".frag");
    QString vertexshader =  KGlobal::dirs()->findResource("data", "kwin/" + name + ".vert");
    if(fragmentshader.isEmpty() || vertexshader.isEmpty())
    {
        kError() << k_funcinfo << "Couldn't locate shader files for '" << name << "'" << endl;
        return false;
    }
    GLShader* shader = new GLShader(vertexshader, fragmentshader);
    if(!shader->isValid())
    {
        kError() << k_funcinfo << "Shader '" << name << "' failed to load!" << endl;
        delete shader;
        return 0;
    }
    return shader;
}

bool BlurEffect::supported()
    {
    return GLRenderTarget::supported() &&
            GLShader::fragmentShaderSupported() &&
            (effects->compositingType() == OpenGLCompositing);
    }


void BlurEffect::prePaintScreen( int* mask, QRegion* region, int time )
    {
    mTransparentWindows = 0;
    mTime += time;

    effects->prePaintScreen(mask, region, time);
    }

void BlurEffect::prePaintWindow( EffectWindow* w, int* mask, QRegion* paint, QRegion* clip, int time )
{
    effects->prePaintWindow( w, mask, paint, clip, time );

    if( w->isPaintingEnabled() && ( *mask & PAINT_WINDOW_TRANSLUCENT ))
        mTransparentWindows++;
}

void BlurEffect::paintWindow( EffectWindow* w, int mask, QRegion region, WindowPaintData& data )
{
    if( mValid && mTransparentWindows )
    {
        if( mask & PAINT_WINDOW_TRANSLUCENT )
        {
            // Make sure the blur texture is up to date
            if( mask & PAINT_SCREEN_TRANSFORMED )
            {
                // We don't want any transformations when working with our own
                //  textures, so load an identity matrix
                glPushMatrix();
                glLoadIdentity();
            }
            // If we're having transformations, we don't know the window's
            //  transformed position on the screen and thus have to update the
            //  entire screen
            if( mask & ( PAINT_WINDOW_TRANSFORMED | PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS ) )
                updateBlurTexture( QRegion(0, 0, displayWidth(), displayHeight()) );
            else
                updateBlurTexture(region);
            if( mask & PAINT_SCREEN_TRANSFORMED )
                // Restore the original matrix
                glPopMatrix();

            // Set custom shader to render the window with
            mWindowShader->bind();
            w->setShader(mWindowShader);
            // Put the blur texture to tex unit 4
            glActiveTexture(GL_TEXTURE4);
            mBlurTexture->bind();
            mBlurTexture->enableUnnormalizedTexCoords();
            glActiveTexture(GL_TEXTURE0);

            // Paint
            effects->paintWindow( w, mask, region, data );
            if(mTransparentWindows > 1)
            {
                // If we have multiple translucent windows on top of each
                //  other, we need to paint those onto the scene rendertarget
                //  as well
                effects->pushRenderTarget(mSceneTarget);
                effects->paintWindow( w, mask, region, data );
                effects->popRenderTarget();
            }

            // Disable blur texture and shader
            glActiveTexture(GL_TEXTURE4);
            mBlurTexture->disableUnnormalizedTexCoords();
            mBlurTexture->unbind();
            glActiveTexture(GL_TEXTURE0);
            mWindowShader->unbind();
        }
        else
        {
            // Opaque window
            // Paint to the screen...
            effects->paintWindow( w, mask, region, data );
            // ...and to the rendertarget as well
            effects->pushRenderTarget(mSceneTarget);
            effects->paintWindow( w, mask, region, data );
            effects->popRenderTarget();
        }
    }
    else
        // If there are no translucent windows then paint as usual
        effects->paintWindow( w, mask, region, data );
}

void BlurEffect::updateBlurTexture(const QRegion& region)
{
    QRect bounding = region.boundingRect();
    QVector<QRect> rects = region.rects();
    int totalarea = 0;
    foreach( QRect r, rects )
        totalarea += r.width() * r.height();
    if( (int)(totalarea * 1.33 + 100 ) < bounding.width() * bounding.height() )
    {
        // Use small rects
        updateBlurTexture(rects);
    }
    else
    {
        // Bounding rect is probably cheaper
        QVector<QRect> tmp( 1, bounding );
        updateBlurTexture( tmp );
    }
}

void BlurEffect::updateBlurTexture(const QVector<QRect>& rects)
{
    // Blur
    // First pass (vertical)
    effects->pushRenderTarget(mTmpTarget);
    mBlurShader->bind();
    mSceneTexture->bind();

    mBlurShader->setAttribute("xBlur", 0);
    mBlurShader->setAttribute("yBlur", 1);

    foreach( QRect r, rects )
        {
        r.adjust(-mBlurRadius, -mBlurRadius, mBlurRadius, mBlurRadius);
        glBegin(GL_QUADS);
            glVertex2f( r.x()            , r.y() + r.height() );
            glVertex2f( r.x() + r.width(), r.y() + r.height() );
            glVertex2f( r.x() + r.width(), r.y() );
            glVertex2f( r.x()            , r.y() );
        glEnd();
        }


    mSceneTexture->unbind();
    mBlurShader->unbind();
    effects->popRenderTarget();

    // Second pass (horizontal)
    effects->pushRenderTarget(mBlurTarget);
    mBlurShader->bind();
    mTmpTexture->bind();

    mBlurShader->setAttribute("xBlur", 1);
    mBlurShader->setAttribute("yBlur", 0);

    foreach( QRect r, rects )
        {
        r.adjust(-mBlurRadius, -mBlurRadius, mBlurRadius, mBlurRadius);
        glBegin(GL_QUADS);
            glVertex2f( r.x()            , r.y() + r.height() );
            glVertex2f( r.x() + r.width(), r.y() + r.height() );
            glVertex2f( r.x() + r.width(), r.y() );
            glVertex2f( r.x()            , r.y() );
        glEnd();
        }


    mTmpTexture->unbind();
    mBlurShader->unbind();
    effects->popRenderTarget();
}

} // namespace

