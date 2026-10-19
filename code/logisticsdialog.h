#ifndef LOGISTICSDIALOG_H
#define LOGISTICSDIALOG_H
#include <memory>
/*************************************************************************************************\
LogisticsDialog.h			: Interface for the LogisticsDialog component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include <string>
#include"logisticsscreen.h"
#endif

#ifndef AANIM_H
#include"aanim.h"
#endif

#ifndef ALISTBOX_H
#include"alistbox.h"
#endif

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
LogisticsDialog:
**************************************************************************************************/
class LogisticsDialog: public LogisticsScreen
{
public:

		virtual void		render();
		virtual void		update();

		void begin();
		void end();

		bool				isDone() { return bDone; }

		// PAUSE-IMGUI-SLICE-3: the ImGui confirm-dialog port drives the dialog
		// state machine without the legacy rect hit-test. updateAnimsOnly()
		// advances the enter/exit anims + the bDone latch only (the input half
		// of update() is replaced by ImGui buttons / a keyboard mirror); the
		// anim accessors let the port replay the legacy slide + fade exactly.
		// Legacy callers keep using update()/render() — behavior unchanged.
		void		updateAnimsOnly();
		aAnimation&	getEnterAnim() { return enterAnim; }
		aAnimation&	getExitAnim() { return exitAnim; }

		void	setFont( int newFontResID );

		LogisticsDialog();




protected:

		aAnimation	enterAnim;
		aAnimation	exitAnim;
		bool		bDone;
		int								oldFont;

};

class LogisticsOKDialog : public LogisticsDialog
{
	public:

		static LogisticsOKDialog* instance(){ return s_instance.get(); }


		LogisticsOKDialog();
		

		virtual int			handleMessage( unsigned long, unsigned long );

		static int init( FitIniFile& file );

		void				setText( int textID, int CancelButton, int OKButton );
		void				setText( const char* mainText );



	private:

		LogisticsOKDialog( const LogisticsOKDialog& src );
		virtual ~LogisticsOKDialog();
		LogisticsOKDialog& operator=( const LogisticsOKDialog& ogisticsDialog );


		friend struct std::default_delete<LogisticsOKDialog>;
		static std::unique_ptr<LogisticsOKDialog>		s_instance;

	 friend void endDialogs();
};

class LogisticsOneButtonDialog : public LogisticsDialog
{
	public:

		static LogisticsOneButtonDialog* instance(){ return s_instance.get(); }


		LogisticsOneButtonDialog();
		

		virtual int			handleMessage( unsigned long, unsigned long );

		static int init( FitIniFile& file );

		void				setText( int textID, int CancelButton, int OKButton );
		void				setText( const char* mainText );



	protected:

		LogisticsOneButtonDialog( const LogisticsOneButtonDialog& src );
		virtual ~LogisticsOneButtonDialog();
		LogisticsOneButtonDialog& operator=( const LogisticsOneButtonDialog& ogisticsDialog );


		friend struct std::default_delete<LogisticsOneButtonDialog>;
		static std::unique_ptr<LogisticsOneButtonDialog>		s_instance;

	 friend void endDialogs();
};

class LogisticsLegalDialog : public LogisticsOneButtonDialog
{
public:
	 
	static LogisticsLegalDialog* instance(){ return s_instance.get(); }
	static int init( FitIniFile& file );

	LogisticsLegalDialog(){}

private:

	LogisticsLegalDialog( const LogisticsLegalDialog& src );
	virtual ~LogisticsLegalDialog(){}
	LogisticsLegalDialog& operator=( const LogisticsLegalDialog& ogisticsDialog );

	friend struct std::default_delete<LogisticsLegalDialog>;
		static std::unique_ptr<LogisticsLegalDialog>		s_instance;

	 friend void endDialogs();

};



class LogisticsSaveDialog : public LogisticsDialog
{
	public:

		static LogisticsSaveDialog* instance(){ return s_instance.get(); }


		LogisticsSaveDialog();

		virtual void begin();
		virtual void beginLoad(bool bSkipSaveScan = false);
		void		 beginCampaign();

		virtual void end();
		virtual void update();
		virtual void render();
		
		static int init( FitIniFile& file );
		virtual int			handleMessage( unsigned long, unsigned long );

		const std::string& getFileName( ){ return selectedName; }



	private:

		LogisticsSaveDialog( const LogisticsSaveDialog& src );
		virtual ~LogisticsSaveDialog();
		LogisticsSaveDialog& operator=( const LogisticsSaveDialog& ogisticsDialog );

		friend struct std::default_delete<LogisticsSaveDialog>;
		static std::unique_ptr<LogisticsSaveDialog>		s_instance;

		aListBox	gameListBox;

		std::string	selectedName;

		void	initDialog(const char* path, bool bCampaign);
		void	updateCampaignMissionInfo();
		void	setMission( const char* path );
		void	readCampaignNameFromFile(const char* fileName, char* resultName, long len );
		bool	isCorrectVersionSaveGame( char *fileName );



		bool	bPromptOverwrite;
		bool	bDeletePrompt;
		bool	bLoad;
		bool	bCampaign;

		void updateMissionInfo();
		aLocalizedListItem templateItem;
		

	 friend void endDialogs();

};

class LogisticsVariantDialog : public LogisticsDialog
{
public:

		LogisticsVariantDialog();
		~LogisticsVariantDialog();
		static LogisticsVariantDialog* instance(){ return s_instance; }

		virtual void begin();
		void	beginTranscript();
		virtual void end();
		virtual void update();
		virtual void render();
		
		int init( FitIniFile& file );
		virtual int			handleMessage( unsigned long, unsigned long );

		const std::string& getFileName( ){ return selectedName; }



protected:

		static LogisticsVariantDialog*		s_instance;
		aListBox	gameListBox;
		std::string	selectedName;

		bool	bPromptOverwrite;
		bool	bDeletePrompt;
		bool	bTranscript;


		void	initVariantList();
		void	initTranscript();


		aAnimTextListItem templateItem;

	 friend void endDialogs();
};

class LogisticsAcceptVariantDialog : public LogisticsVariantDialog
{
public:
		LogisticsAcceptVariantDialog();
		~LogisticsAcceptVariantDialog();
		virtual void begin();
		virtual void end();
		virtual void update();
		virtual void render();
		
		int init( FitIniFile& file );
		virtual int			handleMessage( unsigned long, unsigned long );
 
private:

	std::string	selectedName;
	bool		bNameUsedPrompt;

};

class LogisticsMapInfoDialog : public LogisticsDialog
{
public:
		LogisticsMapInfoDialog();
		~LogisticsMapInfoDialog();
		virtual void end();

		void setMap( const char* pFileName );
		
		int init( );
		virtual int			handleMessage( unsigned long, unsigned long );
 
private:

		

};





//*************************************************************************************************
#endif  // end of file ( LogisticsDialog.h )
