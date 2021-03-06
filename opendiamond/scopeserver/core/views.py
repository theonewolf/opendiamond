#
#  The OpenDiamond Platform for Interactive Search
#
#  Copyright (c) 2010-2011 Carnegie Mellon University
#  All rights reserved.
#
#  This software is distributed under the terms of the Eclipse Public
#  License, Version 1.0 which can be found in the file named LICENSE.
#  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
#  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

from django.contrib.auth.decorators import login_required
from opendiamond.scopeserver import render_response

@login_required
def index(request):
    return render_response(request, "scopeserver/home.html")
