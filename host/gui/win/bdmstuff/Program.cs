using System;
using System.Windows.Forms;

namespace bdmstuff
{
    static class Program
    {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main()
        {
            // Visual styles, text rendering and the csproj's high-DPI mode
            ApplicationConfiguration.Initialize();
            Application.Run(new frmMain());
        }
    }
}
