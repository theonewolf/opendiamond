

/*!  \defgroup searchlet  Searchlet API
The searchlet API is the programming interface between the Diamond and
the Diamond applications running on the host.   All communications with
Diamond are performed through this interface.  The programming interface
is defined in lib_searchlet.h.

*/

/*!  \defgroup Filter  Filter API
The filter API is the API used by the searchlets to interact with
the Diamond run-time when evaluating objects.  It provides interfaces
to read/write object data as well as reading and writing object 
attributes.  This API can also be used by the host application
to manipulate data objects.  The programming interface is defined
in lib_filter.h
 */

/*!

\mainpage Diamond Programming

\section intro_sec Introduction

This is a brief overview of the APIs for Diamond.  In our
system the application has two parts.  The first (called the
applications) runs on the host and interacts with the user
to generate a search, display results, etc.  The application
interacts with Diamond through the searchlet API (see lib_searchlet.h).

The second part of the application is the searchlet.  The searchlet
is binary code and configuration state that is downloaded to
the disk to search through the objects.  Each searchlet is composed
of one or more filters.  The diamond runtime uses these filters
to evaluate each of the objects and decide if it is "interesting".
Filters compute on an object using the object's data or
the object's attributes.  They can also add new attributes
that will persist while the object is being searched (we use
this mechanism to pass state between filters).  Each filter
returns an integer that is compared to a threshold.  If the
value is above the threshold it passes the filter, if the value
is below the threshold the filter has rejected the object.  If
any filters rejects an object, the runtime drops the object and
proceeds to the next object.  If an object passes all the filters
then it is passed to the applications.

The filter code is passed as a shared library and each filter function
is an entry point within this shared library.  We require that the
application link all the required libraries into this shared library
so we don't need to worry about visioning, etc. at the storage device.
The filter specification specifies each entry point to use
as well as parameters that are passed to the filter functions.  Each
filter provides three entry points; an init function, an eval
function, and a fini function.  The init function is called once
per search with any arguments passed in the filter specification.  This
function can build state the is passed to the eval function.  The eval
function is called to evaluate and object, and the fini function is
called when the search completes to clean up any state.

The filters interact with the Diamond runtime to read object contents,
read/write object attributes, etc through the filter API.  This
API is documented in lib_filter.h.


\section app_sec	Application Programming

*/

/*!
\include demo_app.c

\section searchlet_sec	Searchlet Programming

 
*/
